#!/usr/bin/env python3

import base64
import enum
import glob
import inspect
import ipaddress
import psutil
import re
import random
import shutil
import socket
import time
import dns.message
import dns.query
import dns.update
from subprocess import Popen, PIPE, check_call, CalledProcessError, check_output, run, DEVNULL
from dnstest.utils import *
from dnstest.context import Context
import dnstest.config
import dnstest.inquirer
import dnstest.params as params
import dnstest.keys
import dnstest.knsupdate
from dnstest.libknot import libknot
import dnstest.module
import dnstest.response
import dnstest.update
import distutils.dir_util
from shutil import copyfile

def zone_arg_check(zone):
    # Convert one item list to single object.
    if isinstance(zone, list):
        if len(zone) != 1:
            raise Failed("One zone required")
        return zone[0]
    return zone

class ZoneDnssec(object):
    '''Zone DNSSEC signing configuration'''

    def __init__(self):
        self.enable = None
        self.validate = None
        self.disable = None # create the policy in config, but set dnssec-signing: off
        self.manual = None
        self.single_type_signing = None
        self.alg = None
        self.ksk_size = None
        self.zsk_size = None
        self.dnskey_ttl = None
        self.zone_max_ttl = None
        self.keytag_modulo = "0/1"
        self.ksk_lifetime = None
        self.zsk_lifetime = None
        self.delete_delay = None
        self.propagation_delay = None
        self.rrsig_lifetime = None
        self.rrsig_refresh = None
        self.rrsig_prerefresh = None
        self.repro_sign = None
        self.nsec3 = None
        self.nsec3_iters = None
        self.nsec3_opt_out = None
        self.nsec3_salt_lifetime = None
        self.nsec3_salt_len = None
        self.ksk_sbm_check = []
        self.ksk_sbm_check_interval = None
        self.ksk_sbm_timeout = None
        self.ksk_sbm_delay = None
        self.ds_push = None
        self.dnskey_sync = None
        self.ksk_shared = None
        self.shared_policy_with = None
        self.cds_publish = None
        self.cds_digesttype = None
        self.dnskey_mgmt = None
        self.offline_ksk = None
        self.signing_threads = None

class ZoneCatalogRole(enum.IntEnum):
    """Zone catalog roles."""

    NONE = 0
    INTERPRET = 1
    GENERATE = 2
    MEMBER = 3
    HIDDEN = 4 # Interpreted member zone

class Zone(object):
    '''DNS zone description'''

    def __init__(self, zone_file, ddns=False, ixfr=False, journal_content="changes"):
        self.zfile = zone_file
        self.masters = set()
        self.slaves = set()
        self.serial_modulo = None
        self.ddns = ddns
        self.ixfr = ixfr
        self.journal_content = journal_content # journal contents
        self.modules = []
        self.reverse_from = None
        self.dnssec = ZoneDnssec()
        self.catalog_role = ZoneCatalogRole.NONE
        self.catalog_gen_name = None # Generated catalog name for this member
        self.catalog_group = None
        self.refresh_max = None
        self.refresh_min = None
        self.retry_max = None
        self.retry_min = None
        self.expire_max = None
        self.expire_min = None

    @property
    def name(self):
        return self.zfile.name

    def add_module(self, module):
        self.modules.append(module)

    def get_module(self, mod_name):
        for m in self.modules:
            if m.mod_name == mod_name:
               return m

    def clear_modules(self):
        self.modules.clear()

    def disable_master(self, new_zone_file):
        self.zfile.remove()
        self.zfile = new_zone_file
        self.ixfr = False

class Server(object):
    '''Specification of DNS server'''

    START_WAIT = 2
    START_WAIT_VALGRIND = 5
    START_WAIT_ATTEMPTS = 60
    START_MAX_ATTEMPTS = 10  # During the test, fatal.
    START_INIT_ATTEMPTS = 3  # When starting a test, non-fatal.
    STOP_TIMEOUT = 30
    COMPILE_TIMEOUT = 60
    DIG_TIMEOUT = 5

    # Instance counter.
    count = 0

    def __init__(self):
        self.proc = None
        self.valgrind = []
        self.start_params = None
        self.ctl_params = None
        self.ctl_params_append = None # The last parameter wins.

        self.data_dir = None

        self.nsid = None
        self.ident = None
        self.version = None

        self.addr = None
        self.addr_extra = list()
        self.port = 53 # Needed for keymgr when port not yet generated
        self.xdp_port = None # 0 indicates that XDP is enabled but port not yet assigned
        self.xdp_cover_sock = None # dummy socket bound to XDP port just to avoid further port collisions
        self.quic_port = None
        self.tls_port = None
        self.cert_key = str()
        self.cert_key_file = None # quadruple (key_file, cert_file, hostname, pin)
        self.udp_workers = None
        self.tcp_workers = None
        self.bg_workers = None
        self.fixed_port = False
        self.ctlport = None
        self.external = False
        self.ctlkey = None
        self.ctlkeyfile = None
        self.tsig = None
        self.tsig_test = None
        self.no_xfr_edns = None
        self.via = None

        self.zones = dict()

        self.tcp_reuseport = None
        self.tcp_remote_io_timeout = None
        self.tcp_io_timeout = None
        self.tcp_idle_timeout = None
        self.quic_idle_close_timeout = None
        self.udp_max_payload = None
        self.udp_max_payload_ipv4 = None
        self.udp_max_payload_ipv6 = None
        self.disable_notify = None
        self.ddns_master = None
        self.semantic_check = True
        self.zonefile_sync = "1d"
        self.notify_delay = None
        self.zonefile_load = None
        self.zonemd_verify = None
        self.zonemd_generate = None
        self.ixfr_benevolent = None
        self.ixfr_by_one = None
        self.ixfr_from_axfr = None
        self.journal_db_size = 20 * 1024 * 1024
        self.journal_max_usage = 5 * 1024 * 1024
        self.journal_max_depth = 100
        self.timer_db_size = 1 * 1024 * 1024
        self.kasp_db_size = 10 * 1024 * 1024
        self.catalog_db_size = 10 * 1024 * 1024
        self.zone_size_limit = None
        self.serial_policy = None
        self.auto_acl = None
        self.provide_ixfr = None
        self.master_pin_tol = None
        self.quic_log = None

        self.inquirer = None

        self.modules = []

        # Working directory.
        self.dir = None
        # Name of server instance.
        self.name = None
        self.fout = None
        self.ferr = None
        self.valgrind_log = None
        self.session_log = None
        self.confile = None

        self.binding_errors = 0

    def _check_socket(self, proto, port):
        if self.addr.startswith("/"):
            ux_socket = True
            param = ""
            iface = self.addr
        else:
            ux_socket = False
            param = "-i"
            if ipaddress.ip_address(self.addr).version == 4:
                iface = "4%s@%s:%i" % (proto, self.addr, port)
            else:
                iface = "6%s@[%s]:%i" % (proto, self.addr, port)

        for i in range(5):
            pids = []
            proc = Popen(["lsof", param, iface],
                         stdout=PIPE, stderr=PIPE, universal_newlines=True)
            (out, err) = proc.communicate()
            for line in out.split("\n"):
                fields = line.split()
                if len(fields) > 1 and fields[1] != "PID" and fields[-1] != "(ESTABLISHED)":
                    pids.append(fields[1])

            pids = list(set(pids))

            # Check for successful bind.
            if (ux_socket or len(pids) == 1) and str(self.proc.pid) in pids:
                return True

            time.sleep(2)

        return False

    def query_port(self, xdp=None):
        if self.xdp_port is None or self.xdp_port == 0:
            xdp = False
        if xdp is None:
            xdp = (random.random() < 0.8)
        return self.xdp_port if xdp else self.port

    def set_master(self, zone, slave=None, ddns=False, ixfr=False, journal_content="changes"):
        '''Set the server as a master for the zone'''

        if zone.name not in self.zones:
            master_file = zone.clone(self.dir + "/master")
            z = Zone(master_file, ddns, ixfr, journal_content)
            self.zones[zone.name] = z
        else:
            z = self.zones[zone.name]

        if slave:
            z.slaves.add(slave)

    def set_slave(self, zone, master, ddns=False, ixfr=False, journal_content="changes"):
        '''Set the server as a slave for the zone'''

        slave_file = zone.clone(self.dir + "/slave", exists=False)

        if zone.name not in self.zones:
            z = Zone(slave_file, ddns, ixfr, journal_content)
            self.zones[zone.name] = z
        else:
            z = self.zones[zone.name]
            z.disable_master(slave_file)

        z.masters.add(master)

    def cat_interpret(self, zone):
        z = self.zones[zone_arg_check(zone).name]
        z.catalog_role = ZoneCatalogRole.INTERPRET

    def cat_generate(self, zone):
        z = self.zones[zone_arg_check(zone).name]
        z.catalog_role = ZoneCatalogRole.GENERATE

    def cat_member(self, zone, catalog, group=None):
        z = self.zones[zone_arg_check(zone).name]
        c = self.zones[zone_arg_check(catalog).name]
        z.catalog_role = ZoneCatalogRole.MEMBER
        z.catalog_gen_name = c.name
        z.catalog_group = group

    def cat_hidden(self, zone):
        z = self.zones[zone_arg_check(zone).name]
        z.catalog_role = ZoneCatalogRole.HIDDEN

    def compile(self):
        try:
            p = Popen([self.control_bin] + self.compile_params,
                      stdout=self.fout, stderr=self.ferr)
            p.communicate(timeout=Server.COMPILE_TIMEOUT)
        except:
            raise Failed("Can't compile server='%s'" %self.name)

    def wait_for_pidfile(self, attempts=8):
        '''Wait for a PID file to disappear, with a timeout'''

        pidf = os.path.join(self.dir, self.pidfile)
        for i in range(attempts):
            if not os.path.isfile(pidf):
                break
            time.sleep(0.5)

    def start_server(self, clean=False):
        '''Start the server'''
        mode = "w" if clean else "a"

        try:
            if os.path.isfile(self.valgrind_log):
                copyfile(self.valgrind_log, self.valgrind_log + str(int(time.time())))

            if os.path.isfile(self.session_log):
                copyfile(self.session_log, self.session_log + str(int(time.time())))

            if os.path.isfile(self.fout):
                copyfile(self.fout, self.fout + str(int(time.time())))

            if os.path.isfile(self.ferr):
                copyfile(self.ferr, self.ferr + str(int(time.time())))

            if self.daemon_bin != None:
                self.proc = Popen(self.valgrind + [self.daemon_bin] + \
                                  self.start_params,
                                  stdout=open(self.fout, mode=mode),
                                  stderr=open(self.ferr, mode=mode),
                                  env=dict(os.environ, SSLKEYLOGFILE=self.session_log))

            if self.valgrind:
                time.sleep(Server.START_WAIT_VALGRIND)
            else:
                time.sleep(Server.START_WAIT)
        except OSError:
            raise Failed("Can't start server='%s'" % self.name)

        # Start inquirer if enabled.
        if Context().test.stress and self.inquirer:
            self.inquirer.start(self)

    def start(self, clean=False, fatal=True):
        '''Start the server with all bindings successful'''

        max = Server.START_MAX_ATTEMPTS if fatal else Server.START_INIT_ATTEMPTS
        for attempt in range(max):
            self.wait_for_pidfile()
            self.start_server(clean)
            errors = self.log_search_count(self.binding_fail)
            if errors == (0 if clean else self.binding_errors):
                return
            self.binding_errors = errors  # Store it for future attempts.
            if attempt < (max - 1):
                self.stop()
                time.sleep(Server.START_WAIT_ATTEMPTS)
                check_log("STARTING %s AGAIN" % self.name)

        if fatal:
            raise Failed("Server %s couldn't bind all addresses or ports" % self.name)
        else:
            check_log("BUSY PORTS, START OF %s FAILED" % self.name)

    def ctl(self, cmd, wait=False, availability=True, read_result=False):
        if availability:
            # Check for listening control interface.
            ok = False
            for i in range(0, 5):
                try:
                    self.ctl("status", availability=False)
                except Failed:
                    time.sleep(1)
                    continue
                ok = True
                break
            if not ok:
                self.backtrace()
                raise Failed("Unavailable remote control server='%s'" % self.name)

        # Send control command.
        args = self.ctl_params + (self.control_wait if wait else []) + cmd.split()
        try:
            check_call([self.control_bin] + args,
                       stdout=open(self.dir + "/call.out", mode="a"),
                       stderr=open(self.dir + "/call.err", mode="a"))
        except CalledProcessError as e:
            self.backtrace()
            raise Failed("Can't control='%s' server='%s', ret='%i'" %
                         (cmd, self.name, e.returncode))

        # Allow the command to complete, Bind needs this.
        self.wait_function(wait)

        if read_result:
            with open(self.dir + "/call.out", "r") as f:
                return f.readlines()[-1]

    def reload(self):
        self.ctl("reload")
        time.sleep(Server.START_WAIT)

    def running(self):
        proc = psutil.Process(self.proc.pid)
        status = proc.status
        # psutil 2.0.0+ makes status a function
        if psutil.version_info[0] >= 2:
            status = proc.status()
        if status == psutil.STATUS_RUNNING or \
           status == psutil.STATUS_SLEEPING or \
           status == psutil.STATUS_DISK_SLEEP:
            return True
        else:
            return False

    def _valgrind_check(self):
        if not self.valgrind:
            return

        check_log("VALGRIND CHECK %s" % self.name)

        lock = False
        lost = 0
        reachable = 0
        errcount = 0

        try:
            f = open(self.valgrind_log, "r")
        except:
            detail_log("No err log file")
            detail_log(SEP)
            return

        for line in f:
            if re.search("(HEAP|LEAK) SUMMARY", line):
                lost = 0
                reachable = 0
                errcount = 0
                lock = True
                continue

            if lock:
                lost_line = re.search("lost:", line)
                if lost_line:
                    lost += int(line[lost_line.end():].lstrip(). \
                                split(" ")[0].replace(",", ""))
                    continue

                reach_line = re.search("reachable:", line)
                if reach_line:
                    reachable += int(line[reach_line.end():].lstrip(). \
                                     split(" ")[0].replace(",", ""))
                    continue

                err_line = re.search("ERROR SUMMARY:", line)
                if err_line:
                    errcount += int(line[err_line.end():].lstrip(). \
                                    split(" ")[0].replace(",", ""))

                    if lost > 0 or reachable > 0 or errcount > 0:
                        set_err("VALGRIND")
                        detail_log("%s memcheck: lost(%i B), reachable(%i B), " \
                                   "errcount(%i)" \
                                   % (self.name, lost, reachable, errcount))

                    lock = False
                    continue

        detail_log(SEP)
        f.close()

    def _asan_check(self):
        if os.path.isfile(self.ferr) and fsearch(self.ferr, "LeakSanitizer"):
            set_err("LeakSanitizer")

    def _assert_check(self):
        if os.path.isfile(self.ferr) and fsearch(self.ferr, "Assertion"):
            set_err("ASSERT")

    def backtrace(self):
        if self.valgrind:
            check_log("BACKTRACE %s" % self.name)

            try:
                check_call([params.gdb_bin, "-ex", "set confirm off", "-ex",
                            "target remote | %s --pid=%s" %
                            (params.vgdb_bin, self.proc.pid),
                            "-ex", "info threads",
                            "-ex", "thread apply all bt full", "-ex", "q",
                            self.daemon_bin],
                           stdout=open(self.dir + "/gdb.out", mode="a"),
                           stderr=open(self.dir + "/gdb.err", mode="a"))
            except:
                detail_log("!Failed to get backtrace")

            detail_log(SEP)

    def stop(self, check=True):
        if Context().test.stress and self.inquirer:
            self.inquirer.stop()

        if self.proc:
            try:
                self.proc.terminate()
                self.proc.wait(Server.STOP_TIMEOUT)
            except ProcessLookupError:
                pass
            except:
                self.backtrace()
                check_log("WARNING: KILLING %s" % self.name)
                detail_log(SEP)
                self.kill()
        if check:
            self._assert_check()
            self._valgrind_check()
            self._asan_check()

    def kill(self):
        if Context().test.stress and self.inquirer:
            self.inquirer.stop()

        if self.proc:
            # Store PID before kill.
            pid = self.proc.pid

            self.proc.kill()

            # Remove uncleaned vgdb pipes.
            for f in glob.glob("/tmp/vgdb-pipe*-%s-*" % pid):
                try:
                    os.remove(f)
                except:
                    pass

    def gen_confile(self):
        if os.path.isfile(self.confile):
            copyfile(self.confile, self.confile + str(int(time.time())))

        f = open(self.confile, mode="w")
        f.write(self.get_config())
        f.close()

    def fill_cert_key(self):
        try:
            out = check_output([self.control_bin] + self.ctl_params + ["status", "cert-key"],
                               stderr=open(self.dir + "/call.err", mode="a"))
            key = out.rstrip().decode('ascii')
            if key != "-":
                self.cert_key = key
        except CalledProcessError as e:
            raise Failed("Can't get certificate key, server='%s', ret='%i'" %
                         (self.name, e.returncode))

    def use_default_cert_key(self, key_name="key.pem", cert_name="cert.pem"):
        for f in [key_name, cert_name]:
            shutil.copy(os.path.join(params.common_data_dir, "cert", f), self.dir)
        keyfile = os.path.join(self.dir, key_name)
        certfile = os.path.join(self.dir, cert_name)

        try:
            out = check_output(["certtool", "--infile=" + keyfile, "-k"]).rstrip().decode('ascii')
            pin = ssearch(out, r'pin-sha256:([^\n]*)')
            out = check_output(["certtool", "--infile=" + certfile, "-i"]).rstrip().decode('ascii')
            hostname = ssearch(out, r'DNSname: ([^\n]*)')
            self.cert_key_file = (keyfile, certfile, hostname, pin)
            return pin
        except CalledProcessError as e:
            raise Failed("Can't use default certificate and key")

    def download_cert_file(self, dest_dir):
        try:
            check_call(["gnutls-cli", "--help"], stdout=DEVNULL, stderr=DEVNULL)
        except:
            raise Skip("gnutls-cli not available")

        certfile = os.path.join(dest_dir, "%s_tls_%d.crt" % (self.name, int(time.time())))
        cmd = Popen(["gnutls-cli", self.addr, "-p", str(self.tls_port or self.quic_port),
                     "--no-ca-verification", "-V", "--save-cert=" + certfile],
                     stdin=DEVNULL, stdout=PIPE, stderr=DEVNULL)
        (out, _) = cmd.communicate()
        gcli_s = out.decode("utf-8")
        hostname1 = ssearch(gcli_s, r'DNSname: ([^\n]*)')
        hostname2 = ssearch(gcli_s, r'Subject:.*CN=([^,\n]*)')
        hostname3 = socket.gethostname()
        return ("", certfile, hostname1 or hostname2 or hostname3, ssearch(gcli_s, r'pin-sha256:([^\n]*)'))

    def dig(self, rname, rtype, rclass="IN", udp=None, serial=None, timeout=None,
            tries=3, flags="", bufsize=None, edns=None, nsid=False, dnssec=False,
            log_no_sep=False, tsig=None, addr=None, source=None, xdp=None):

        # Convert one item zone list to zone name.
        if isinstance(rname, list):
            if len(rname) != 1:
                raise Failed("One zone required")
            rname = rname[0].name

        opcode = dns.opcode.QUERY
        rtype_str = rtype.upper()

        # Set port type.
        if rtype.upper() == "AXFR":
            # Always use TCP.
            udp = False
        elif rtype.upper() == "IXFR":
            # Use TCP if not specified.
            udp = udp if udp != None else False
            rtype_str += "=%i" % int(serial)
        if rtype.upper() == "NOTIFY":
            rtype = "SOA"
            rtype_str = "SOA"
            opcode = dns.opcode.NOTIFY
        else:
            # Use TCP or UDP at random if not specified.
            udp = udp if udp != None else random.choice([True, False])

        if udp:
            dig_flags = "+notcp"
        else:
            dig_flags = "+tcp"

        dig_flags += " +retry=%i" % (tries - 1)

        # Set timeout.
        if timeout is None:
            timeout = self.DIG_TIMEOUT
        dig_flags += " +time=%i" % timeout

        # Prepare query (useless for XFR).
        query = dns.message.make_query(rname, rtype, rclass)
        query.set_opcode(opcode)

        # Remove implicit RD flag.
        query.flags &= ~dns.flags.RD

        # Set packet flags.
        flag_names = flags.split()
        for flag in flag_names:
            if flag == "AA":
                query.flags |= dns.flags.AA
                dig_flags += " +aa"
            elif flag == "TC":
                query.flags |= dns.flags.TC
                dig_flags += " +tc"
            elif flag == "RD":
                query.flags |= dns.flags.RD
                dig_flags += " +rd"
            elif flag == "RA":
                query.flags |= dns.flags.RA
                dig_flags += " +ra"
            elif flag == "AD":
                query.flags |= dns.flags.AD
                dig_flags += " +ad"
            elif flag == "CD":
                query.flags |= dns.flags.CD
                dig_flags += " +cd"
            elif flag == "Z":
                query.flags |= 64
                dig_flags += " +z"

        # Set EDNS.
        if edns != None or bufsize or nsid:
            class NsidFix(object):
                '''Old pythondns doesn't implement NSID option.'''
                def __init__(self):
                    self.otype = dns.edns.NSID
                def to_wire(self, file=None):
                    pass

            if edns:
                edns = int(edns)
            else:
                edns = 0
            dig_flags += " +edns=%i" % edns

            if bufsize:
                payload = int(bufsize)
            else:
                payload = 1232
            dig_flags += " +bufsize=%i" % payload

            if nsid:
                if not hasattr(dns, 'version') or dns.version.MAJOR == 1:
                    options = [NsidFix()]
                else:
                    options = [dns.edns.GenericOption(dns.edns.NSID, b'')]
                dig_flags += " +nsid"
            else:
                options = None

            query.use_edns(edns=edns, payload=payload, options=options)

        # Set DO flag.
        if dnssec:
            query.want_dnssec()
            dig_flags += " +dnssec +bufsize=%i" % query.payload

        # Store function arguments for possible comparison.
        args = dict()
        params = inspect.getargvalues(inspect.currentframe())
        for param in params.args:
            if param != "self":
                args[param] = params.locals[param]

        if addr is None:
            addr = self.addr

        # Add source to dig flags if present
        if source is not None:
            dig_flags += " -b " + source

        check_log("DIG %s %s %s @%s -p %i %s" %
                  (rname, rtype_str, rclass, addr, self.port, dig_flags))

        # Set TSIG for a normal query if explicitly specified.
        key_params = dict()
        if tsig != None:
            if type(tsig) is dnstest.keys.Tsig:
                key_params = tsig.key_params
            elif tsig and self.tsig_test:
                key_params = self.tsig_test.key_params
        if key_params:
            query.use_tsig(keyring=key_params["keyring"],
                           keyname=key_params["keyname"],
                           algorithm=key_params["keyalgorithm"])

        # Set TSIG for a transfer if available.
        if rtype.upper() == "AXFR" or rtype.upper() == "IXFR":
            if self.tsig_test and tsig != False:
                key_params = self.tsig_test.key_params

        if key_params:
            detail_log("%s:%s:%s" %
                (key_params["keyalgorithm"], key_params["keyname"],
                 base64.b64encode(list(key_params["keyring"].values())[0]).decode('ascii')))

        for t in range(tries):
            try:
                if rtype.upper() == "AXFR":
                    resp = dns.query.xfr(addr, rname, rtype, rclass,
                                         port=self.query_port(xdp), lifetime=timeout,
                                         use_udp=udp, **key_params)
                elif rtype.upper() == "IXFR":
                    resp = dns.query.xfr(addr, rname, rtype, rclass,
                                         port=self.query_port(xdp), lifetime=timeout,
                                         use_udp=udp, serial=int(serial),
                                         **key_params)
                elif udp:
                    resp = dns.query.udp(query, addr, port=self.query_port(xdp),
                                         timeout=timeout, source=source)
                else:
                    resp = dns.query.tcp(query, addr, port=self.query_port(xdp),
                                         timeout=timeout, source=source)

                if not log_no_sep:
                    detail_log(SEP)

                return dnstest.response.Response(self, resp, query, args)
            except dns.exception.Timeout:
                pass
            except Exception as e:
                detail_log("DIG returned: %s" % e)
                if t < tries - 1:
                    time.sleep(timeout)

        raise Failed("Can't query server='%s' for '%s %s %s'" % \
                     (self.name, rname, rclass, rtype))

    def create_sock(self, socket_type):
        family = socket.AF_INET
        if ipaddress.ip_address(self.addr).version == 6:
            family = socket.AF_INET6
        return socket.socket(family, socket_type)

    def send_raw(self, data, sock=None):
        if sock is None:
            sock = self.create_sock(socket.SOCK_DGRAM)
        sent = sock.sendto(bytes(data, 'utf-8'), (self.addr, self.port))
        if sent != len(data):
            raise Failed("Can't send RAW data (%d bytes) to server='%s'" %
                         (len(data), self.name))

    def log_search(self, pattern):
        return fsearch(self.fout, pattern) or fsearch(self.ferr, pattern)

    def log_search_count(self, pattern):
        return fsearch_count(self.fout, pattern) + fsearch_count(self.ferr, pattern)

    def zone_wait(self, zone, serial=None, equal=False, greater=True, udp=True,
                  tsig=None, use_ctl=False):
        '''Try to get SOA record. With an optional serial number and given
           relation (equal or/and greater).'''

        zone = zone_arg_check(zone)

        _serial = 0

        if use_ctl:
            ctl = libknot.control.KnotCtl()
            zone_name = zone.name.lower()

        check_log("ZONE WAIT %s: %s" % (self.name, zone.name))

        attempts = 60 if not self.valgrind else 100
        for t in range(attempts):
            try:
                if use_ctl:
                    ctl.connect(os.path.join(self.dir, "knot.sock"))
                    ctl.send_block(cmd="zone-read", zone=zone_name,
                                   owner="@", rtype="SOA")
                    resp = ctl.receive_block()
                    ctl.send(libknot.control.KnotCtlType.END)
                    ctl.close()
                    soa_rdata = resp[zone_name][zone_name]["SOA"]["data"][0]
                    _serial = int(soa_rdata.split()[2])
                else:
                    resp = self.dig(zone.name, "SOA", udp=udp, tries=1,
                                    timeout=2, log_no_sep=True, tsig=tsig)
                    soa = str((resp.resp.answer[0]).to_rdataset())
                    _serial = int(soa.split()[5])
            except:
                pass
            else:
                if not serial:
                    break
                elif equal and serial == _serial:
                    break
                elif greater and serial < _serial:
                    break
            time.sleep(2)
        else:
            self.backtrace()
            serial_str = ""
            if serial:
                serial_str = "%s%s%i" % (">" if greater else "",
                                         "=" if equal else "", serial)
            raise Failed("Can't get SOA%s, zone='%s', server='%s'" %
                         (serial_str, zone.name, self.name))

        detail_log(SEP)

        return _serial

    def zones_wait(self, zone_list, serials=None, serials_zfile=False, equal=False,
                   greater=True, use_ctl=False):
        new_serials = dict()

        if serials_zfile:
            if serials is not None:
                raise Exception('serials_zfile incompatible with serials')
            serials = dict()
            for zone in zone_list:
                serials[zone.name] = self.zones[zone.name].zfile.get_soa_serial()

        for zone in zone_list:
            old_serial = serials[zone.name] if serials else None
            new_serial = self.zone_wait(zone, serial=old_serial, equal=equal,
                                        greater=greater, use_ctl=use_ctl)
            new_serials[zone.name] = new_serial

        return new_serials

    def zone_backup(self, zone, flush=False):
        zone = zone_arg_check(zone)

        if flush:
            self.flush(zone=zone, wait=True)

        self.zones[zone.name].zfile.backup()

    def zone_verify(self, zone, bind_check=True, ldns_check=True):
        zone = zone_arg_check(zone)

        self.zones[zone.name].zfile.dnssec_verify(bind_check, ldns_check)

    def check_nsec(self, zone, nsec3=False, nonsec=False):
        zone = zone_arg_check(zone)

        resp = self.dig("0-x-not-existing-x-0." + zone.name, "ANY", dnssec=True)
        resp.check_nsec(nsec3=nsec3, nonsec=nonsec)

    def update(self, zone, allow_knsupdate=True):
        zone = zone_arg_check(zone)

        key_params = self.tsig_test.key_params if self.tsig_test else dict()

        if allow_knsupdate and random.choice([False, True]):
            return dnstest.update.Update(self, dnstest.knsupdate.Knsupdate(zone.name, self.tsig_test))
        else:
            return dnstest.update.Update(self, dns.update.Update(zone.name, **key_params))

    def gen_key(self, zone, **args):
        zone = zone_arg_check(zone)

        key = dnstest.keys.Key(self.confile, zone.name, **args)
        key.generate()

        return key

    @property
    def keydir(self):
        d = os.path.join(self.dir, "keys")
        if not os.path.exists(d):
            os.makedirs(d)
        return d

    def use_keys(self, zone):
        zone = zone_arg_check(zone)
        # copy all keys, even for other zones
        distutils.dir_util.copy_tree(zone.key_dir, self.keydir, update=True)

    def dnssec(self, zone):
        zone = zone_arg_check(zone)

        return self.zones[zone.name].dnssec

    def enable_nsec3(self, zone, **args):
        zone = zone_arg_check(zone)

        self.zones[zone.name].zfile.enable_nsec3(**args)

    def disable_nsec3(self, zone):
        zone = zone_arg_check(zone)

        self.zones[zone.name].zfile.disable_nsec3()

    def update_zonefile(self, zone, version=None, random=False, storage=None):
        zone = zone_arg_check(zone)

        if not storage:
            storage = self.data_dir

        if random:
            self.zones[zone.name].zfile.update_rnd()
        else:
            self.zones[zone.name].zfile.upd_file(storage=storage, version=version)

    def random_ddns(self, zone, allow_empty=True, allow_ns=True, tries=20):
        zone = zone_arg_check(zone)

        for i in range(tries):
            up = self.update(zone)

            while True:
                changes = self.zones[zone.name].zfile.gen_rnd_ddns(up, allow_ns)
                if allow_empty or changes > 0:
                    break

            if up.try_send() == "NOERROR":
                return

        raise Failed("Can't send DDNS update of zone '%s' to server '%s'" % (zone.name, self.name))

    def add_module(self, zone, module):
        zone = zone_arg_check(zone)

        if zone:
            self.zones[zone.name].add_module(module)
        else:
            self.modules.append(module)

    def clear_modules(self, zone):
        zone = zone_arg_check(zone)

        if zone:
            self.zones[zone.name].clear_modules()
        else:
            self.modules.clear()

    def clean(self, zone=True, timers=True):
        if zone:
            zone = zone_arg_check(zone)

            # Remove all zonefiles.
            if zone is True:
                for _z in sorted(self.zones):
                    zfile = self.zones[_z].zfile.path
                    try:
                        os.remove(zfile)
                    except:
                        pass
            # Remove specified zonefile.
            else:
                zfile = self.zones[zone.name].zfile.path
                try:
                    os.remove(zfile)
                except:
                    pass

        if timers:
            try:
                shutil.rmtree(self.dir + "/timers")
            except:
                pass

class Bind(Server):

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        if not params.bind_bin:
            raise Skip("No Bind")
        self.daemon_bin = params.bind_bin
        self.control_bin = params.bind_ctl
        self.control_wait = []
        self.ctlkey = dnstest.keys.Tsig(alg="hmac-md5")
        self.binding_fail = "address in use"
        self.pidfile = "bind.pid"

    def listening(self):
        tcp = super()._check_socket("tcp", self.port)
        udp = super()._check_socket("udp", self.port)
        ctltcp = super()._check_socket("tcp", self.ctlport)
        return (tcp and udp and ctltcp)

    def wait_function(self, wait=False):
        # There's no blocking mode in rndc, simulating it.
        time.sleep(Server.START_WAIT + (3 if wait else 0))

    def flush(self, zone=None, wait=False):
        zone_name = (" " + zone.name) if zone else ""
        self.ctl("sync%s" % zone_name, wait=wait)

    def bind_version(self):
        return tuple(map(int, run([self.daemon_bin, '-v'], stdout=PIPE, stderr=PIPE, text=True).stdout.replace('BIND ', '').split('-')[0].split('.')))

    def check_option(self, option):
        proc = Popen([params.bind_checkconf_bin, "/dev/fd/0"],
                     stdout=PIPE, stderr=PIPE, stdin=PIPE,
                     universal_newlines=True)
        conff = "options {\n    %s;\n};" % option
        (out, err) = proc.communicate(input=conff)
        return proc.wait() == 0

    def _key(self, conf, keys, key, comment):
        if key and key.name not in keys:
            conf.begin("key", key.name)
            conf.comment("%s" % comment)
            conf.item("algorithm", key.alg)
            conf.item_str("secret", key.key)
            conf.end()
            keys.add(key.name)

    def _str(self, conf, name, value):
        if value and value != True:
            conf.item_str(name, value)

    def _int(self, conf, name, value):
        if value is not None:
            conf.item(name, str(value))

    def get_config(self):
        s = dnstest.config.BindConf()

        if self.tls_port and self.cert_key_file:
            s.begin("tls self")
            s.item_str("key-file", self.cert_key_file[0])
            s.item_str("cert-file", self.cert_key_file[1])
            s.end()

        s.begin("options")
        self._str(s, "server-id", self.ident)
        self._str(s, "version", self.version)
        s.item_str("directory", self.dir)
        s.item_str("key-directory", self.dir)
        s.item_str("managed-keys-directory", self.dir)
        s.item_str("session-keyfile", self.dir + "/session.key")
        s.item_str("pid-file", os.path.join(self.dir, self.pidfile))
        if ipaddress.ip_address(self.addr).version == 4:
            s.item("listen-on port", "%i { %s; }" % (self.port, self.addr))
            if self.tls_port:
                s.item("listen-on port", "%i tls %s { %s; }" % \
                       (self.tls_port, "self" if self.cert_key_file else "ephemeral", self.addr))
            s.item("listen-on-v6", "{ }")
        else:
            s.item("listen-on-v6 port", "%i { %s; }" % (self.port, self.addr))
            if self.tls_port:
                s.item("listen-on-v6 port", "%i tls %s { %s; }" % \
                       (self.tls_port, "self" if self.cert_key_file else "ephemeral", self.addr))
            s.item("listen-on", "{ }")
        s.item("auth-nxdomain", "no")
        s.item("recursion", "no")
        s.item("masterfile-format", "text")
        s.item("masterfile-style", "full")
        s.item("max-refresh-time", "2")
        s.item("max-retry-time", "2")
        s.item("transfers-in", "30")
        s.item("transfers-out", "30")
        s.item("minimal-responses", "true")
        s.item("notify-delay", "0")
        s.item("notify-rate", "1000")
        s.item("max-journal-size", "unlimited")
        if self.check_option("max-ixfr-ratio unlimited"):
            s.item("max-ixfr-ratio", "unlimited")
        s.item("startup-notify-rate", "1000")
        s.item("serial-query-rate", "1000")
        s.end()

        s.begin("key", self.ctlkey.name)
        s.item("algorithm", self.ctlkey.alg)
        s.item_str("secret", self.ctlkey.key)
        s.end()

        s.begin("controls")
        s.item("inet %s port %i allow { %s; } keys { %s; }"
               % (self.addr, self.ctlport, Context().test.addr, self.ctlkey.name))
        s.end()

        masters_and_slaves = set()
        for zone in sorted(self.zones):
            z = self.zones[zone]
            masters_and_slaves.update(z.masters)
            masters_and_slaves.update(z.slaves)
        for rmt in masters_and_slaves:
            if rmt.tls_port and rmt.cert_key_file:
                s.begin("tls %s" % rmt.name)
                s.item_str("ca-file", rmt.cert_key_file[1])
                s.item_str("remote-hostname", rmt.cert_key_file[2])
                s.end()

        if self.tsig:
            keys = set() # Duplicy check.
            self._key(s, keys, self.tsig_test, "test")
            self._key(s, keys, self.tsig, "local")

            for zone in sorted(self.zones):
                z = self.zones[zone]
                for master in z.masters:
                    self._key(s, keys, master.tsig, master.name)
                for slave in z.slaves:
                    self._key(s, keys, slave.tsig, slave.name)

        for zone in sorted(self.zones):
            z = self.zones[zone]
            if not z.dnssec.enable:
                continue

            s.begin("dnssec-policy", z.name)
            s.begin("keys")
            zsk_life = "unlimited" if z.dnssec.zsk_lifetime is None else z.dnssec.zsk_lifetime
            ksk_life = "unlimited" if z.dnssec.ksk_lifetime is None else z.dnssec.ksk_lifetime
            alg = "ecdsa256" if z.dnssec.alg is None else z.dnssec.alg
            if z.dnssec.single_type_signing:
                s.item("csk", "lifetime %s algorithm %s" % (ksk_life, alg))
            else:
                s.item("zsk", "lifetime %s algorithm %s" % (zsk_life, alg))
                s.item("ksk", "lifetime %s algorithm %s" % (ksk_life, alg))
            s.end()
            self._int(s, "dnskey-ttl", z.dnssec.dnskey_ttl)
            self._int(s, "max-zone-ttl", z.dnssec.zone_max_ttl)
            self._int(s, "zone-propagation-delay", z.dnssec.propagation_delay)
            self._int(s, "signatures-validity", z.dnssec.rrsig_lifetime)
            self._int(s, "signatures-refresh", z.dnssec.rrsig_refresh)
            if self.bind_version() >= (9, 18, 28):
                s.item("signatures-jitter", "0")
            s.item("publish-safety", "1")
            s.item("retire-safety", "1")
            s.item("parent-ds-ttl", "5")
            s.item("parent-propagation-delay", "5")
            s.end()

        for zone in sorted(self.zones):
            z = self.zones[zone]
            s.begin("zone", z.name)
            s.item_str("file", z.zfile.path)
            s.item("check-names", "warn")

            if z.masters:
                s.item("type", "slave")

                masters = ""
                masters_notify = ""
                for master in z.masters:
                    if self.tsig:
                        masters += "%s port %i key %s" \
                                   % (master.addr, master.tls_port or master.port, self.tsig.name)
                        if not master.disable_notify:
                            masters_notify += "key %s; " % master.tsig.name
                    else:
                        masters += "%s port %i" \
                                   % (master.addr, master.tls_port or master.port)
                        if not master.disable_notify:
                            masters_notify += "%s; " % master.addr
                    if master.tls_port:
                        masters += " tls %s" % (master.name if master.cert_key_file else "ephemeral")
                    masters += "; "
                s.item("masters", "{ %s}" % masters)
                if masters_notify:
                    s.item("allow-notify", "{ %s}" % masters_notify)
            else:
                s.item("type", "master")
                s.item("notify", "explicit")
                s.item("check-integrity", "no")

            if z.ixfr and not z.masters:
                s.item("ixfr-from-differences", "yes")

            if z.slaves:
                slaves = ""
                for slave in z.slaves:
                    if self.disable_notify:
                        continue
                    if self.tsig:
                        slaves += "%s port %s key %s" \
                                  % (slave.addr, slave.port, self.tsig.name)
                    else:
                        slaves += "%s port %s" % (slave.addr, slave.port)
                    #if slave.tls_port:
                    #    slaves += " tls %s" % (slave.name if slave.cert_key_file else "ephemeral")
                    # TODO Bind9 fails to send NOTIFYoverTLS, until fixed https://gitlab.isc.org/isc-projects/bind9/-/issues/4821
                    slaves += "; "
                if slaves:
                    s.item("also-notify", "{ %s}" % slaves)

            if z.ddns:
                if self.tsig_test:
                    upd = "key %s; " % self.tsig_test.name
                else:
                    upd = "%s; " % Context().test.addr

                if z.masters:
                    s.item("allow-update-forwarding", "{ %s}" % upd)
                else:
                    s.item("allow-update", "{ %s}" % upd)

            slaves_xfr = ""
            if self.tsig or self.tsig_test:
                slaves_xfr = "key %s" % self.tsig_test.name
                for slave in z.slaves:
                    slaves_xfr += "; key %s" % slave.tsig.name
            else:
                slaves_xfr = "any"
            s.item("allow-transfer", "{ %s; }" % slaves_xfr )

            if z.dnssec.enable:
                s.item("inline-signing", "yes")
                s.item("dnssec-policy", z.name)
                s.item_str("key-directory", self.keydir)
                if z.dnssec.ksk_sbm_check:
                    parents = ""
                    for parent in z.dnssec.ksk_sbm_check:
                        parents += "%s port %i; " % (parent.addr, parent.port)
                    s.item("parental-agents", "{ %s}" % parents)
            s.end()

        self.start_params = ["-c", self.confile, "-g"]
        self.ctl_params = ["-s", self.addr, "-p", str(self.ctlport), \
                           "-k", self.ctlkeyfile]

        return s.conf

    def start(self, clean=False, fatal=True):
        for zname in self.zones:
            z = self.zones[zname]
            if z.dnssec.enable != True:
                continue

            # unrelated: generate keys as Bind won't do
            ps = [ 'dnssec-keygen', '-n', 'ZONE', '-a', 'ECDSA256', '-K', self.keydir ]
            if z.dnssec.nsec3:
                ps += ['-3']
            k1 = check_output(ps + [z.name], stderr=DEVNULL)
            k2 = check_output(ps + ["-f", "KSK"] + [z.name], stderr=DEVNULL)

            k1 = self.keydir + '/' + k1.rstrip().decode('ascii')
            k2 = self.keydir + '/' + k2.rstrip().decode('ascii')

            # Append to zone
            with open(z.zfile.path, 'a') as outf:
                outf.write('\n')
                with open(k1 + '.key', 'r') as kf:
                    for line in kf:
                        if len(line) > 0 and line[0] != ';':
                            outf.write(line)
                with open(k2 + '.key', 'r') as kf:
                    for line in kf:
                        if len(line) > 0 and line[0] != ';':
                            outf.write(line)
                #if z.dnssec.nsec3:
                    #n3flag =  1 if z.dnssec.nsec3_opt_out else 0
                    #n3iters = z.dnssec.nsec3_iters or 0
                    #outf.write("%s NSEC3PARAM 1 %d %d -\n" % (z.name, n3flag, n3iters)) # this does not work!

        super().start(clean, fatal)

        for zname in self.zones:
            z = self.zones[zname]
            if z.dnssec.nsec3:
                n3flag =  1 if z.dnssec.nsec3_opt_out else 0
                n3iters = z.dnssec.nsec3_iters or 0
                self.ctl("signing -nsec3param 1 %d %d - %s" % (n3flag, n3iters, z.name))

class Knot(Server):

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        if not params.knot_bin:
            raise Skip("No Knot")
        self.daemon_bin = params.knot_bin
        self.control_bin = params.knot_ctl
        self.control_wait = ["-b"]
        self.inquirer = dnstest.inquirer.Inquirer()
        self.includes = set()
        self.binding_fail = "cannot bind address"
        self.pidfile = "knot.pid"

    def listening(self):
        tcp = super()._check_socket("tcp", self.port)
        udp = super()._check_socket("udp", self.port)
        return (tcp and udp)

    def wait_function(self, wait=False): # needed for compatibility with Bind class
        pass

    def flush(self, zone=None, wait=False):
        params = "-f " if str(self.zonefile_sync)[0] == '-' else ""
        if zone:
            self.ctl("%szone-flush %s" % (params, zone.name), wait=wait)
        else:
            self.ctl("%szone-flush" % params, wait=wait)

    def key_gen(self, zone_name, **new_params):
        set_params = [ option + "=" + value for option, value in new_params.items() ]
        res = dnstest.keys.Keymgr.run_check(self.confile, zone_name, "generate", *set_params)
        errcode, stdo, stde = res
        return stdo.split()[-1]

    def key_set(self, zone_name, key_id, **new_values):
        set_params = [ option + "=" + value for option, value in new_values.items() ]
        dnstest.keys.Keymgr.run_check(self.confile, zone_name, "set", key_id, *set_params)

    def key_import_bind(self, zone_name):
        if zone_name not in self.zones:
            assert(0)
        bind_keydir = self.zones[zone_name].zfile.key_dir_bind
        assert(zone_name.endswith("."))
        for pkey_path in glob.glob("%s/K*.private" % glob.escape(bind_keydir)):
            pkey = os.path.basename(pkey_path)
            m = re.match(r'K(?P<name>[^+]+)\+(?P<algo>\d+)\+(?P<tag>\d+)\.private', pkey)
            if m and m.group("name") == zone_name.lower():
                dnstest.keys.Keymgr.run_check(self.confile, zone_name, "import-bind", pkey_path)

    def _on_str_hex(self, conf, name, value):
        if value == True:
            return
        elif value == False:
            conf.item_str(name, "")
        elif value:
            conf.item_str(name, value)

    def _key(self, conf, keys, key, comment):
        if key and key.name not in keys:
            conf.id_item("id", key.name)
            conf.comment("%s" % comment)
            conf.item_str("algorithm", key.alg)
            conf.item_str("secret", key.key)
            keys.add(key.name)

    def _bool(self, conf, name, value):
        if value != None:
            conf.item_str(name, "on" if value else "off")

    def _str(self, conf, name, value):
        if value != None:
            conf.item_str(name, value)

    def data_add(self, file_name, storage=None):
        if storage == ".":
            src_dir = self.data_dir
        elif storage:
            src_dir = storage
        else:
            src_dir = params.common_data_dir

        src_file = src_dir + file_name
        dst_file = self.dir + '/' + file_name
        shutil.copyfile(src_file, dst_file)

        return dst_file

    def include(self, file_name, storage=None, empty=False):
        if empty:
            self.includes.add(file_name)
        else:
            dst_file = self.data_add(file_name, storage)
            self.includes.add(dst_file)

    def first_master(self, zone_name):
        return sorted(self.zones[zone_name].masters, key=lambda srv: srv.name)[0]

    def config_xfr(self, zone, conf):
        acl = "acl_test"
        if zone.masters:
            masters = ""
            for master in sorted(zone.masters, key=lambda srv: srv.name):
                if masters:
                    masters += ", "
                masters += master.name
                if not master.disable_notify:
                    acl += ", acl_%s" % master.name
            conf.item("master", "[%s]" % masters)
        if zone.slaves:
            slaves = ""
            for slave in sorted(zone.slaves, key=lambda srv: srv.name):
                if not self.disable_notify:
                    if slaves:
                        slaves += ", "
                    slaves += slave.name
                acl += ", acl_%s" % slave.name
            if slaves:
                conf.item("notify", "[%s]" % slaves)
        for remote in zone.dnssec.dnskey_sync if zone.dnssec.dnskey_sync else []:
            acl += ", acl_%s_ddns" % remote.name
        if not self.auto_acl:
            conf.item("acl", "[%s]" % acl)

    def get_config(self):
        s = dnstest.config.KnotConf()

        for file in self.includes:
            s.include(file)

        s.begin("server")
        self._on_str_hex(s, "identity", self.ident)
        self._on_str_hex(s, "version", self.version)
        self._on_str_hex(s, "nsid", self.nsid)
        s.item_str("rundir", self.dir)
        s.item_str("pidfile", os.path.join(self.dir, self.pidfile))
        if self.addr.startswith("/"):
            s.item_str("listen", "%s" % self.addr)
        else:
            s.item_str("listen", "%s@%s" % (self.addr, self.port))
        if self.quic_port:
            s.item_str("listen-quic", "%s@%s" % (self.addr, self.quic_port))
        if self.tls_port:
            s.item_str("listen-tls", "%s@%s" % (self.addr, self.tls_port))
        if self.udp_workers:
            s.item_str("udp-workers", self.udp_workers)
        if self.tcp_workers:
            s.item_str("tcp-workers", self.tcp_workers)
        if self.bg_workers:
            s.item_str("background-workers", self.bg_workers)

        for addr in self.addr_extra:
            s.item_str("listen", "%s@%s" % (addr, self.port))
        self._bool(s, "tcp-reuseport", self.tcp_reuseport)
        self._str(s, "tcp-remote-io-timeout", self.tcp_remote_io_timeout)
        self._str(s, "tcp-io-timeout", self.tcp_io_timeout)
        self._str(s, "tcp-idle-timeout", self.tcp_idle_timeout)
        self._str(s, "quic-idle-close-timeout", self.quic_idle_close_timeout)
        self._str(s, "udp-max-payload", self.udp_max_payload)
        self._str(s, "udp-max-payload-ipv4", self.udp_max_payload_ipv4)
        self._str(s, "udp-max-payload-ipv6", self.udp_max_payload_ipv6)
        self._str(s, "remote-pool-limit", str(random.randint(0,6)))
        self._str(s, "remote-retry-delay", str(random.choice([0, 1, 5])))
        self._bool(s, "automatic-acl", self.auto_acl)
        if self.cert_key_file:
            s.item_str("key-file", self.cert_key_file[0])
            s.item_str("cert-file", self.cert_key_file[1])
        s.end()

        if self.xdp_port is not None and self.xdp_port > 0:
            s.begin("xdp")
            s.item_str("listen", "%s@%s" % (self.addr, self.xdp_port))
            s.item_str("tcp", "on")
            if self.quic_port:
                s.item_str("quic", "on")
                s.item_str("quic-port", self.quic_port)
            s.end()

        s.begin("control")
        s.item_str("listen", "knot.sock")
        s.item_str("timeout", "15")
        s.end()

        if self.tsig:
            keys = set() # Duplicy check.
            s.begin("key")
            self._key(s, keys, self.tsig_test, "test")
            self._key(s, keys, self.tsig, "local")

            for zone in sorted(self.zones):
                z = self.zones[zone]
                for master in z.masters:
                    self._key(s, keys, master.tsig, master.name)
                for slave in z.slaves:
                    self._key(s, keys, slave.tsig, slave.name)
                for remote in z.dnssec.dnskey_sync if z.dnssec.dnskey_sync else []:
                    self._key(s, keys, remote.tsig, remote.name)
            s.end()

        have_remote = False
        servers = set() # Duplicity check.
        for zone in sorted(self.zones):
            z = self.zones[zone]
            for master in z.masters:
                if master.name not in servers:
                    if not have_remote:
                        s.begin("remote")
                        have_remote = True
                    s.id_item("id", master.name)
                    if master.quic_port or master.tls_port:
                        s.item_str("address", "%s@%s" % (master.addr, master.tls_port or master.quic_port))
                        s.item_str("tls" if master.tls_port else "quic", "on")
                        if master.cert_key:
                            s.item_str("cert-key", master.cert_key)
                        elif master.cert_key_file:
                            s.item_str("cert-key", master.cert_key_file[3])
                    else:
                        if master.addr.startswith("/"):
                            s.item_str("address", "%s" % master.addr)
                        else:
                            s.item_str("address", "%s@%s" % (master.addr, master.query_port()))
                    if self.tsig:
                        s.item_str("key", self.tsig.name)
                    if self.via:
                        s.item_str("via", self.via)
                    if master.no_xfr_edns:
                        s.item_str("no-edns", "on")
                    servers.add(master.name)
            for slave in z.slaves:
                if slave.name not in servers:
                    if not have_remote:
                        s.begin("remote")
                        have_remote = True
                    s.id_item("id", slave.name)
                    if slave.quic_port or slave.tls_port:
                        s.item_str("address", "%s@%s" % (slave.addr, slave.tls_port or slave.quic_port))
                        s.item_str("tls" if slave.tls_port else "quic", "on")
                        if slave.cert_key:
                            s.item_str("cert-key", slave.cert_key)
                        elif slave.cert_key_file:
                            s.item_str("cert-key", slave.cert_key_file[3])
                    else:
                        if slave.addr.startswith("/"):
                            s.item_str("address", "%s" % slave.addr)
                        else:
                            s.item_str("address", "%s@%s" % (slave.addr, slave.query_port()))
                    if self.via:
                        s.item_str("via", self.via)
                    if self.tsig:
                        s.item_str("key", self.tsig.name)
                    servers.add(slave.name)
            for parent in z.dnssec.ksk_sbm_check + [ z.dnssec.ds_push ] if z.dnssec.ds_push else z.dnssec.ksk_sbm_check:
                if isinstance(parent, Server) and parent.name not in servers:
                    if not have_remote:
                        s.begin("remote")
                        have_remote = True
                    s.id_item("id", parent.name)
                    if parent.addr.startswith("/"):
                        s.item_str("address", "%s" % parent.addr)
                    else:
                        s.item_str("address", "%s@%s" % (parent.addr, parent.port))
                    if self.via:
                        s.item_str("via", self.via)
                    servers.add(parent.name)
            for remote in z.dnssec.dnskey_sync if z.dnssec.dnskey_sync else []:
                if remote.name not in servers:
                    if not have_remote:
                        s.begin("remote")
                        have_remote = True
                    s.id_item("id", remote.name)
                    if remote.quic_port or remote.tls_port:
                        s.item_str("address", "%s@%s" % (remote.addr, remote.tls_port or remote.quic_port))
                        s.item_str("tls" if remote.tls_port else "quic", "on")
                        if remote.cert_key:
                            s.item_str("cert-key", remote.cert_key)
                        elif remote.cert_key_file:
                            s.item_str("cert-key", remote.cert_key_file[3])
                    else:
                        if remote.addr.startswith("/"):
                            s.item_str("address", "%s" % remote.addr)
                        else:
                            s.item_str("address", "%s@%s" % (remote.addr, remote.port))
                    if remote.via:
                        s.item_str("via", self.via)
                    if remote.tsig:
                        s.item_str("key", self.tsig.name)
                    servers.add(remote.name)

        if have_remote:
            s.end()

        s.begin("acl")
        s.id_item("id", "acl_test")
        s.item_str("address", Context().test.addr)
        if self.tsig_test:
            s.item_str("key", self.tsig_test.name)
        s.item("action", "[transfer, notify, update]")

        servers = set() # Duplicity check.
        for zone in sorted(self.zones):
            z = self.zones[zone]
            for master in z.masters:
                if master.name not in servers:
                    s.id_item("id", "acl_%s" % master.name)
                    if master.addr.startswith("/"):
                        s.item_str("address", self.addr)
                    else:
                        s.item_str("address", master.addr)
                    if master.tsig:
                        s.item_str("key", master.tsig.name)
                    if master.cert_key and not isinstance(master, Bind): # TODO until fixed https://gitlab.isc.org/isc-projects/bind9/-/issues/4821
                        s.item_str("cert-key", master.cert_key)
                    s.item("action", "notify")
                    servers.add(master.name)
            for slave in z.slaves:
                if slave.name in servers:
                    continue
                s.id_item("id", "acl_%s" % slave.name)
                if slave.addr.startswith("/"):
                    s.item_str("address", self.addr)
                else:
                    s.item_str("address", slave.addr)
                if slave.tsig:
                    s.item_str("key", slave.tsig.name)
                if slave.cert_key:
                    s.item_str("cert-key", slave.cert_key)
                s.item("action", "[transfer" + (", update" if z.ddns else "") + "]")
                servers.add(slave.name)
            for remote in z.dnssec.dnskey_sync if z.dnssec.dnskey_sync else []:
                dupl_name = remote.name + "_ddns"
                if dupl_name in servers:
                    continue
                s.id_item("id", "acl_%s" % dupl_name)
                if remote.tsig:
                    s.item_str("key", remote.tsig.name)
                s.item("action", "update")
                servers.add(dupl_name)
        s.end()

        if len(self.modules) > 0:
            for module in self.modules:
                module.get_conf(s)

        for zone in sorted(self.zones):
            z = self.zones[zone]
            if len(z.modules) > 0:
                for module in z.modules:
                    module.get_conf(s)

        have_sbm = False
        for zone in sorted(self.zones):
            z = self.zones[zone]
            if not z.dnssec.enable:
                continue
            if len(z.dnssec.ksk_sbm_check) < 1 and z.dnssec.ksk_sbm_timeout is None:
                continue
            if not have_sbm:
                s.begin("submission")
                have_sbm = True
            s.id_item("id", z.name)
            parents = ""
            for parent in z.dnssec.ksk_sbm_check:
                if parents:
                    parents += ", "
                parents += parent.name
            if parents != "":
                s.item("parent", "[%s]" % parents)
            self._str(s, "check-interval", z.dnssec.ksk_sbm_check_interval)
            if z.dnssec.ksk_sbm_timeout is not None:
                self._str(s, "timeout", z.dnssec.ksk_sbm_timeout)
            if z.dnssec.ksk_sbm_delay is not None:
                self._str(s, "parent-delay", z.dnssec.ksk_sbm_delay)
        if have_sbm:
            s.end()

        have_dnskeysync = False
        for zone in sorted(self.zones):
            z = self.zones[zone]
            if not z.dnssec.enable:
                continue
            if z.dnssec.dnskey_sync is None:
                continue
            if not have_dnskeysync:
                s.begin("dnskey-sync")
                have_dnskeysync = True
            s.id_item("id", z.name)
            remotes = ""
            for remote in z.dnssec.dnskey_sync:
                if remotes:
                    remotes += ", "
                remotes += remote.name
            if remotes != "":
                s.item("remote", "[%s]" % remotes)
        if have_dnskeysync:
            s.end()

        have_policy = False
        for zone in sorted(self.zones):
            z = self.zones[zone]
            if not z.dnssec.enable and not z.dnssec.validate:
                continue

            if (z.dnssec.shared_policy_with or z.name) != z.name:
                continue

            if not have_policy:
                s.begin("policy")
                have_policy = True
            s.id_item("id", z.name)
            self._bool(s, "manual", z.dnssec.manual)
            self._bool(s, "single-type-signing", z.dnssec.single_type_signing)
            self._str(s, "algorithm", z.dnssec.alg)
            self._str(s, "ksk-size", z.dnssec.ksk_size)
            self._str(s, "zsk-size", z.dnssec.zsk_size)
            self._str(s, "dnskey-ttl", z.dnssec.dnskey_ttl)
            self._str(s, "zone-max-ttl", z.dnssec.zone_max_ttl)
            self._str(s, "keytag-modulo", z.dnssec.keytag_modulo)
            self._str(s, "ksk-lifetime", z.dnssec.ksk_lifetime)
            self._str(s, "zsk-lifetime", z.dnssec.zsk_lifetime)
            self._str(s, "delete-delay", z.dnssec.delete_delay)
            self._str(s, "propagation-delay", z.dnssec.propagation_delay)
            self._str(s, "rrsig-lifetime", z.dnssec.rrsig_lifetime)
            self._str(s, "rrsig-refresh", z.dnssec.rrsig_refresh)
            self._str(s, "rrsig-pre-refresh", z.dnssec.rrsig_prerefresh)
            self._str(s, "reproducible-signing", z.dnssec.repro_sign)
            self._bool(s, "nsec3", z.dnssec.nsec3)
            self._str(s, "nsec3-iterations", z.dnssec.nsec3_iters)
            self._bool(s, "nsec3-opt-out", z.dnssec.nsec3_opt_out)
            self._str(s, "nsec3-salt-lifetime", z.dnssec.nsec3_salt_lifetime)
            self._str(s, "nsec3-salt-length", z.dnssec.nsec3_salt_len)
            if len(z.dnssec.ksk_sbm_check) > 0 or z.dnssec.ksk_sbm_timeout is not None:
                s.item("ksk-submission", z.name)
            if z.dnssec.dnskey_sync:
                s.item("dnskey-sync", z.name)
            self._bool(s, "ksk-shared", z.dnssec.ksk_shared)
            self._str(s, "cds-cdnskey-publish", z.dnssec.cds_publish)
            if z.dnssec.cds_digesttype:
                self._str(s, "cds-digest-type", z.dnssec.cds_digesttype)
            self._str(s, "dnskey-management", z.dnssec.dnskey_mgmt)
            self._bool(s, "offline-ksk", z.dnssec.offline_ksk)
            self._str(s, "signing-threads",
                      z.dnssec.signing_threads if z.dnssec.signing_threads is not None
                      else str(random.randint(1,4)))
        if have_policy:
            s.end()

        s.begin("database")
        s.item_str("storage", self.dir)
        s.item_str("kasp-db", self.keydir)
        s.item_str("kasp-db-max-size", self.kasp_db_size)
        s.item_str("journal-db-max-size", self.journal_db_size)
        s.item_str("timer-db-max-size", self.timer_db_size)
        s.item_str("catalog-db-max-size", self.catalog_db_size)
        s.end()

        s.begin("template")
        s.id_item("id", "default")
        s.item_str("storage", self.dir)
        s.item_str("zonefile-sync", self.zonefile_sync)
        if self.notify_delay is None:
            self.notify_delay = random.randint(0, 2)
        s.item_str("notify-delay", self.notify_delay)
        if self.zonemd_verify:
            s.item_str("zonemd-verify", "on")
        if self.zonemd_generate is not None:
            s.item_str("zonemd-generate", self.zonemd_generate)
        s.item_str("journal-max-usage", self.journal_max_usage)
        s.item_str("journal-max-depth", self.journal_max_depth)
        s.item_str("adjust-threads", str(random.randint(1,4)))
        if self.semantic_check == "soft":
            self._str(s, "semantic-checks", self.semantic_check)
        else:
            self._bool(s, "semantic-checks", self.semantic_check)
        if len(self.modules) > 0:
            modules = ""
            for module in self.modules:
                if modules:
                    modules += ", "
                modules += module.get_conf_ref()
            s.item("global-module", "[%s]" % modules)
        if self.zone_size_limit:
            s.item("zone-max-size", self.zone_size_limit)

        have_catalog = None
        for zone in self.zones:
            z = self.zones[zone]
            if z.catalog_role in [ZoneCatalogRole.INTERPRET, ZoneCatalogRole.GENERATE]:
                have_catalog = z
                break
        if have_catalog is not None:
            s.id_item("id", "catalog-default")
            s.item_str("file", self.dir + "/catalog/%s.zone")
            s.item_str("zonefile-load", "difference")
            s.item_str("journal-content", z.journal_content)

            # this is weird but for the sake of testing, the cataloged zones inherit dnssec policy from catalog zone
            if z.dnssec.enable:
                s.item_str("dnssec-signing", "off" if z.dnssec.disable else "on")
                s.item_str("dnssec-policy", z.name)
            for module in z.modules:
                if module.conf_name == "mod-onlinesign":
                    s.item("module", "[%s]" % module.get_conf_ref())

            self.config_xfr(z, s)

            s.id_item("id", "catalog-signed")
            s.item_str("file", self.dir + "/catalog/%s.zone")
            s.item_str("journal-content", z.journal_content)
            s.item_str("dnssec-signing", "on")
            self.config_xfr(z, s)

            s.id_item("id", "catalog-unsigned")
            s.item_str("file", self.dir + "/catalog/%s.zone")
            s.item_str("journal-content", z.journal_content)
            self.config_xfr(z, s)

        s.end()

        s.begin("zone")
        for zone in sorted(self.zones):
            z = self.zones[zone]
            if z.catalog_role == ZoneCatalogRole.HIDDEN:
                continue

            s.id_item("domain", z.name)
            s.item_str("file", z.zfile.path)

            self.config_xfr(z, s)

            self._str(s, "serial-policy", self.serial_policy)
            self._str(s, "serial-modulo", z.serial_modulo)
            self._str(s, "ddns-master", self.ddns_master)

            s.item_str("journal-content", z.journal_content)
            self._bool(s, "ixfr-benevolent", self.ixfr_benevolent)
            self._bool(s, "ixfr-by-one", self.ixfr_by_one)
            self._bool(s, "ixfr-from-axfr", self.ixfr_from_axfr)
            self._bool(s, "provide-ixfr", self.provide_ixfr)

            if z.reverse_from:
                s.item("reverse-generate", "[ " + ", ".join([ x.name for x in z.reverse_from ]) + " ]")

            self._str(s, "refresh-min-interval", z.refresh_min)
            self._str(s, "refresh-max-interval", z.refresh_max)
            self._str(s, "retry-min-interval", z.retry_min)
            self._str(s, "retry-max-interval", z.retry_max)
            self._str(s, "expire-min-interval", z.expire_min)
            self._str(s, "expire-max-interval", z.expire_max)

            if self.zonefile_load is not None:
                s.item_str("zonefile-load", self.zonefile_load)
            elif z.ixfr:
                s.item_str("zonefile-load", "difference")

            self._str(s, "master-pin-tolerance", self.master_pin_tol)

            if z.catalog_role == ZoneCatalogRole.GENERATE:
                s.item_str("catalog-role", "generate")
            elif z.catalog_role == ZoneCatalogRole.MEMBER:
                s.item_str("catalog-role", "member")
                s.item_str("catalog-zone", z.catalog_gen_name)
                self._str(s, "catalog-group", z.catalog_group)
            elif z.catalog_role == ZoneCatalogRole.INTERPRET:
                s.item_str("catalog-role", "interpret")
                s.item("catalog-template", "[ catalog-default, catalog-signed, catalog-unsigned ]")

            if z.dnssec.enable:
                s.item_str("dnssec-signing", "off" if z.dnssec.disable else "on")

            if z.dnssec.enable or z.dnssec.validate:
                s.item_str("dnssec-policy", z.dnssec.shared_policy_with or z.name)

            self._bool(s, "dnssec-validation", z.dnssec.validate)

            if z.dnssec.ds_push == "":
                s.item("ds-push", "[ ]")
            elif z.dnssec.ds_push:
                self._str(s, "ds-push", z.dnssec.ds_push.name)

            if len(z.modules) > 0:
                modules = ""
                for module in z.modules:
                    if modules:
                        modules += ", "
                    modules += module.get_conf_ref()
                s.item("module", "[%s]" % modules)
        s.end()

        s.begin("log")
        s.id_item("target", "stdout")
        s.item_str("any", "debug")
        s.id_item("target", self.quic_log)
        s.item_str("quic", "debug")
        s.end()

        self.start_params = ["-c", self.confile]
        self.ctl_params = ["-c", self.confile, "-t", "15"]
        if self.ctl_params_append != None:
            self.ctl_params += self.ctl_params_append

        return s.conf

    def check_quic(self):
        res = run([self.daemon_bin, '-VV'], stdout=PIPE)
        for line in res.stdout.decode('ascii').split("\n"):
            if "DoQ support" in line and "no" not in line:
                return
        raise Skip("QUIC support not available")

class Dummy(Server):
    ''' Dummy name server. '''

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.daemon_bin = None
        self.control_bin = None
        self.control_wait = []
        self.binding_fail = "There won't be such a message"
        self.pidfile = None

    def get_config(self):
        return ''

    def start(self, clean=None, fatal=None):
        return True

    def listening(self):
        return True # Fake listening

    def wait_function(self, wait=False):
        pass

    def running(self):
        return True # Fake running
