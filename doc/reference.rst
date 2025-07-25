.. highlight:: none
.. _Configuration Reference:

***********************
Configuration Reference
***********************

.. _Description:

Description
===========

Configuration files for Knot DNS use simplified YAML format. Simplified means
that not all of the features are supported.

For the description of configuration items, we have to declare a meaning of
the following symbols:

- ``INT`` – Integer
- ``STR`` – Textual string
- ``HEXSTR`` – Hexadecimal string (with ``0x`` prefix)
- ``BOOL`` – Boolean value (``on``/``off`` or ``true``/``false``)
- ``TIME`` – Number of seconds, an integer with a possible time multiplier suffix
  (``s`` ~ 1, ``m`` ~ 60, ``h`` ~ 3600, ``d`` ~ 24 * 3600, ``w`` ~ 7 * 24 * 3600,
  ``M`` ~ 30 * 24 * 3600, ``y`` ~ 365 * 24 * 3600)
- ``SIZE`` – Number of bytes, an integer with a possible size multiplier suffix
  (``B`` ~ 1, ``K`` ~ 1024, ``M`` ~ 1024^2 or ``G`` ~ 1024^3)
- ``BASE64`` – Base64 encoded string
- ``ADDR`` – IPv4 or IPv6 address
- ``DNAME`` – Domain name
- ``...`` – Multi-valued item, order of the values is preserved
- ``[`` ``]`` – Optional value
- ``|`` – Choice

The configuration consists of several fixed sections and optional module
sections. There are 17 fixed sections (``module``, ``server``, ``xdp``, ``control``,
``log``, ``statistics``, ``database``, ``keystore``, ``key``, ``remote``,
``remotes``, ``acl``, ``submission``, ``dnskey-sync``, ``policy``, ``template``,
``zone``).
Module sections are prefixed with the ``mod-`` prefix (e.g. ``mod-stats``).

Most of the sections (e.g. ``zone``) are sequences of settings blocks. Each
settings block begins with a unique identifier, which can be used as a reference
from other sections (such an identifier must be defined in advance).

A multi-valued item can be specified either as a YAML sequence::

 address: [10.0.0.1, 10.0.0.2]

or as more single-valued items each on an extra line::

 address: 10.0.0.1
 address: 10.0.0.2

If an item value contains spaces or other special characters, it is necessary
to enclose such a value within double quotes ``"`` ``"``.

.. _default_paths:

If not specified otherwise, an item representing a file or a directory path may
be defined either as an absolute path (starting with ``/``), or a path relative
to the same directory as the default value of the item.

.. _Comments:

Comments
========

A comment begins with a ``#`` character and is ignored during processing.
Also each configuration section or sequence block allows a permanent
comment using the ``comment`` item which is stored in the server beside the
configuration.

.. _including configuration:

Including configuration
=======================

Another configuration file or files, matching a pattern, can be included at
the top level in the current file.

::

 include: STR

.. _include:

include
-------

A path or a matching pattern specifying one or more files that are included
at the place of the include option position in the configuration.
If the path is not absolute, then it is considered to be relative to the
current file. The pattern can be an arbitrary string meeting POSIX *glob*
requirements, e.g. dir/\*.conf. Matching files are processed in sorted order.

*Default:* not set

.. _clearing configuration sections:

Clearing configuration sections
===============================

It's possible to clear specified configuration sections at given phases
of the configuration parsing.

::

 clear: STR

.. _clear:

clear
-----

A matching pattern specifying configuration sections that are cleared when
this item is parsed. This allows overriding of existing configuration
in the configuration database when including a configuration file or
ensures that some configuration wasn't specified in previous includes.

.. NOTE::
   For the pattern matching the POSIX function
   `fnmatch() <https://pubs.opengroup.org/onlinepubs/9699919799/functions/fnmatch.html>`_
   is used. On Linux, the GNU extension
   `FNM_EXTMATCH <https://www.gnu.org/software/libc/manual/html_node/Wildcard-Matching.html#index-FNM_005fEXTMATCH>`_
   is enabled, which allows extended pattern matching.
   Examples:

   - ``clear: zone`` – Clears the ``zone`` section.
   - ``clear: mod-*`` – Clears all module sections.
   - ``clear: "[!z]*"`` – Clears all sections not beginning with letter ``z``.
   - ``clear: !(zone)`` – (GNU only) Clears all sections except the ``zone`` one.
   - ``clear: @(zone|template)`` – (GNU only) Clears the ``zone`` and ``template`` sections.

*Default:* not set

.. _module section:

``module`` section
==================

Dynamic modules loading configuration.

.. NOTE::
   If configured with non-empty ``--with-moduledir=path`` parameter, all
   shared modules in this directory will be automatically loaded.

::

 module:
   - id: STR
     file: STR

.. _module_id:

id
--

A module identifier in the form of the ``mod-`` prefix and module name suffix.

.. _module_file:

file
----

A path to a shared library file with the module implementation.

.. WARNING::
   If the path is not absolute, the library is searched in the set of
   system directories. See ``man dlopen`` for more details.

*Default:* ``${libdir}/knot/modules-${version}``/module_name.so
(or ``${path}``/module_name.so if configured with ``--with-moduledir=path``)

.. _server section:

``server`` section
==================

General options related to the server.

::

 server:
     identity: [STR]
     version: [STR]
     nsid: [STR|HEXSTR]
     rundir: STR
     user: STR[:STR]
     pidfile: STR
     udp-workers: INT
     tcp-workers: INT
     background-workers: INT
     async-start: BOOL
     tcp-idle-timeout: TIME
     tcp-io-timeout: INT
     tcp-remote-io-timeout: INT
     tcp-max-clients: INT
     tcp-reuseport: BOOL
     tcp-fastopen: BOOL
     quic-max-clients: INT
     quic-outbuf-max-size: SIZE
     quic-idle-close-timeout: TIME
     remote-pool-limit: INT
     remote-pool-timeout: TIME
     remote-retry-delay: INT
     socket-affinity: BOOL
     udp-max-payload: SIZE
     udp-max-payload-ipv4: SIZE
     udp-max-payload-ipv6: SIZE
     key-file: STR
     cert-file: STR
     edns-client-subnet: BOOL
     answer-rotation: BOOL
     automatic-acl: BOOL
     proxy-allowlist: ADDR[/INT] | ADDR-ADDR ...
     dbus-event: none | running | zone-updated | ksk-submission | dnssec-invalid ...
     dbus-init-delay: TIME
     listen: ADDR[@INT] | STR ...
     listen-quic: ADDR[@INT] ...
     listen-tls: ADDR[@INT] ...

.. CAUTION::
   When you change configuration parameters dynamically or via configuration file
   reload, some parameters in the Server section require restarting the Knot server
   so that the changes take effect. See below for the details.

.. _server_identity:

identity
--------

An identity of the server returned in the response to the query for TXT
record ``id.server.`` or ``hostname.bind.`` in the CHAOS class (:rfc:`4892`).
Set to an empty value to disable.

*Default:* FQDN hostname

.. _server_version:

version
-------

A version of the server software returned in the response to the query
for TXT record ``version.server.`` or ``version.bind.`` in the CHAOS
class (:rfc:`4892`). Set to an empty value to disable.

*Default:* server version

.. _server_nsid:

nsid
----

A DNS name server identifier (:rfc:`5001`). Set to an empty value to disable.

*Default:* FQDN hostname at the moment of the daemon start

.. _server_rundir:

rundir
------

A path for storing run-time data (PID file, unix sockets, etc.). A non-absolute
path is relative to the :doc:`knotd<man_knotd>` startup directory.

Depending on the usage of this parameter, its change may require restart of the Knot
server to take effect.

*Default:* ``${localstatedir}/run/knot`` (configured with ``--with-rundir=path``)

.. _server_user:

user
----

A system user with an optional system group (``user:group``) under which the
server is run after starting and binding to interfaces. Linux capabilities
are employed if supported.

Change of this parameter requires restart of the Knot server to take effect.

*Default:* ``root:root``

.. _server_pidfile:

pidfile
-------

A PID file :ref:`location<default_paths>`.

Change of this parameter requires restart of the Knot server to take effect.

*Default:* :ref:`rundir<server_rundir>`\ ``/knot.pid``

.. _server_udp-workers:

udp-workers
-----------

A number of UDP workers (threads) used to process incoming queries
over UDP.

Change of this parameter requires restart of the Knot server to take effect.

*Default:* equal to the number of online CPUs

.. _server_tcp-workers:

tcp-workers
-----------

A number of TCP workers (threads) used to process incoming queries
over TCP.

Change of this parameter requires restart of the Knot server to take effect.

*Default:* equal to the number of online CPUs, default value is at least 10

.. _server_background-workers:

background-workers
------------------

A number of workers (threads) used to execute background operations (zone
loading, zone updates, etc.).

Change of this parameter requires restart of the Knot server to take effect.

*Default:* equal to the number of online CPUs, default value is at most 10

.. _server_async-start:

async-start
-----------

If enabled, server doesn't wait for the zones to be loaded and starts
responding immediately with SERVFAIL answers until the zone loads.

*Default:* ``off``

.. _server_tcp-idle-timeout:

tcp-idle-timeout
----------------

Maximum idle time (in seconds) between requests on an inbound TCP connection.
It means if there is no activity on an inbound TCP connection during this limit,
the connection is closed by the server.

*Minimum:* ``1``

*Default:* ``10``

.. _server_tcp-io-timeout:

tcp-io-timeout
--------------

Maximum time (in milliseconds) to receive or send one DNS message over an inbound
TCP connection. It means this limit applies to normal DNS queries and replies,
incoming DDNS, and **outgoing zone transfers**. The timeout is measured since some
data is already available for processing.
Set to 0 for infinity.

*Default:* ``500`` (milliseconds)

.. CAUTION::
   In order to reduce the risk of Slow Loris attacks, it's recommended setting
   this limit as low as possible on public servers.

.. _server_tcp-remote-io-timeout:

tcp-remote-io-timeout
---------------------

Maximum time (in milliseconds) to receive or send one DNS message over an outbound
TCP connection which has already been established to a configured remote server.
It means this limit applies to incoming zone transfers, sending NOTIFY,
DDNS forwarding, and DS check or push. This timeout includes the time needed
for a network round-trip and for a query processing by the remote.
Set to 0 for infinity.

*Default:* ``5000`` (milliseconds)

.. _server_tcp-reuseport:

tcp-reuseport
-------------

If enabled, each TCP worker listens on its own socket and the OS kernel
socket load balancing is employed using SO_REUSEPORT (or SO_REUSEPORT_LB
on FreeBSD). Due to the lack of one shared socket, the server can offer
higher response rate processing over TCP. However, in the case of
time-consuming requests (e.g. zone transfers of a TLD zone), enabled reuseport
may result in delayed or not being responded client requests. So it is
advisable to use this option on secondary servers.

.. NOTE::
   This option is ignored for UNIX sockets.

Change of this parameter requires restart of the Knot server to take effect.

*Default:* ``off``

.. _server_tcp-fastopen:

tcp-fastopen
------------

If enabled, use TCP Fast Open for outbound TCP communication (client side):
incoming zone transfers, sending NOTIFY, and DDNS forwarding. This mode simplifies
TCP handshake and can result in better networking performance. TCP Fast Open
for inbound TCP communication (server side) isn't affected by this
configuration as it's enabled automatically if supported by OS.

.. NOTE::
   The TCP Fast Open support must also be enabled on the OS level:

   * Linux/macOS: ensure kernel parameter ``net.ipv4.tcp_fastopen`` is ``2`` or
     ``3`` for server side, and ``1`` or ``3`` for client side.
   * FreeBSD: ensure kernel parameter ``net.inet.tcp.fastopen.server_enable``
     is ``1`` for server side, and ``net.inet.tcp.fastopen.client_enable`` is
     ``1`` for client side.

*Default:* ``off``

.. _server_quic-max-clients:

quic-max-clients
----------------

A maximum number of QUIC clients connected in parallel.

See also :ref:`xdp_quic`.

Change of this parameter requires restart of the Knot server to take effect.

*Minimum:* ``128``

*Default:* ``10000`` (ten thousand)

.. _server_quic-outbuf-max-size:

quic-outbuf-max-size
--------------------

Maximum cumulative size of memory used for buffers of unACKed
sent messages. This limit is per one UDP worker.

.. NOTE::
   Set low if little memory is available (together with :ref:`server_quic-max-clients`
   since QUIC connections are memory-heavy). Set to high value if outgoing zone
   transfers of big zone over QUIC are expected.

Change of this parameter requires restart of the Knot server to take effect.

*Minimum:* ``1M`` (1 MiB)

*Default:* ``100M`` (100 MiB)

.. _server_quic-idle-close-timeout:

quic-idle-close-timeout
-----------------------

Time in seconds, after which any idle QUIC connection is gracefully closed.

Change of this parameter requires restart of the Knot server to take effect.

*Minimum:* ``1``

*Default:* ``4``

.. _server_remote-pool-limit:

remote-pool-limit
-----------------

If nonzero, the server will keep up to this number of outgoing TCP connections
open for later use. This is an optimization to avoid frequent opening of
TCP connections to the same remote.

Change of this parameter requires restart of the Knot server to take effect.

*Default:* ``0``

.. _server_remote-pool-timeout:

remote-pool-timeout
-------------------

The timeout in seconds after which the unused kept-open outgoing TCP connections
to remote servers are closed.

*Default:* ``5``

.. _server_remote-retry-delay:

remote-retry-delay
------------------

When a connection attempt times out to some remote address, this information will be
kept for this specified time (in milliseconds) and other connections to the same address won't
be attempted. This prevents repetitive waiting for timeout on an unreachable remote.

*Default:* ``0``

.. _server_socket-affinity:

socket-affinity
---------------

If enabled and if SO_REUSEPORT is available on Linux, all configured network
sockets are bound to UDP and TCP workers in order to increase the networking performance.
This mode isn't recommended for setups where the number of network card queues
is lower than the number of UDP or TCP workers.

Change of this parameter requires restart of the Knot server to take effect.

*Default:* ``off``

.. _server_tcp-max-clients:

tcp-max-clients
---------------

A maximum number of TCP clients connected in parallel, set this below the file
descriptor limit to avoid resource exhaustion.

.. NOTE::
   It is advisable to adjust the maximum number of open files per process in your
   operating system configuration.

*Default:* one half of the file descriptor limit for the server process

.. _server_udp-max-payload:

udp-max-payload
---------------

Maximum EDNS0 UDP payload size default for both IPv4 and IPv6.

*Default:* ``1232``

.. _server_udp-max-payload-ipv4:

udp-max-payload-ipv4
--------------------

Maximum EDNS0 UDP payload size for IPv4.

*Default:* ``1232``

.. _server_udp-max-payload-ipv6:

udp-max-payload-ipv6
--------------------

Maximum EDNS0 UDP payload size for IPv6.

*Default:* ``1232``

.. _server_key-file:

key-file
--------

Path to a server key PEM file which is used for DNS over QUIC/TLS communication.
A non-absolute path of a user specified key file is relative to the
:file:`@config_dir@` directory.

*Default:* auto-generated key

.. _server_cert-file:

cert-file
---------

Path to a server certificate PEM file which is used for DNS over QUIC/TLS communication.
A non-absolute path is relative to the :file:`@config_dir@` directory.

*Default:* one-time in-memory certificate

.. _server_edns-client-subnet:

edns-client-subnet
------------------

Enable or disable EDNS Client Subnet support. If enabled, responses to queries
containing the EDNS Client Subnet option
always contain a valid EDNS Client Subnet option according to :rfc:`7871`.

*Default:* ``off``

.. _server_answer-rotation:

answer-rotation
---------------

Enable or disable sorted-rrset rotation in the answer section of normal replies.
The rotation shift is simply determined by a query ID.

*Default:* ``off``

.. _server_automatic-acl:

automatic-acl
-------------

If enabled, :ref:`automatic ACL<remote_automatic-acl>` setting of
configured remotes is considered when evaluating authorized operations.

*Default:* ``off``

.. _server_proxy-allowlist:

proxy-allowlist
---------------

An ordered list of IP addresses, network subnets, or network ranges
which are allowed as a source address of proxied DNS traffic over UDP.
The supported proxy protocol is
`haproxy PROXY v2 <https://www.haproxy.org/download/2.5/doc/proxy-protocol.txt>`_.

.. NOTE::
   TCP is not supported.

*Default:* not set

.. _server_dbus-event:

dbus-event
----------

Specification of server or zone states which emit a D-Bus signal on the system
bus. The bus name is ``cz.nic.knotd``, the object path is ``/cz/nic/knotd``, and
the interface name is ``cz.nic.knotd.events``.

Possible values:

- ``none`` – No signal is emitted.
- ``running`` – There are two possible signals emitted:

  - ``started`` when the server is started and all configured zones (including
    catalog zones and their members) are loaded or successfully bootstrapped.
  - ``stopped`` when the server shutdown sequence is initiated.
- ``zone-updated`` – The signal ``zone_updated`` is emitted when a zone has been updated;
  the signal parameters are `zone name` and `zone SOA serial`.
- ``keys-updated`` - The signal ``keys_updated`` is emitted when a DNSSEC key set
  is updated; the signal parameter is `zone name`.
- ``ksk-submission`` – The signal ``zone_ksk_submission`` is emitted if there is
  a ready KSK present when the zone is signed; the signal parameters are
  `zone name`, `KSK keytag`, and `KSK KASP id`.
- ``dnssec-invalid`` – The signal ``zone_dnssec_invalid`` is emitted when DNSSEC
  validation fails, or when ZONEMD verification fails; the signal parameters
  are `zone name`, and `remaining seconds` until an RRSIG expires.

.. NOTE::
   This function requires systemd version at least 221 or libdbus.

.. TIP::
   A few sample script templates can be found in
   `the project repository <https://gitlab.nic.cz/knot/knot-dns/-/tree/master/samples>`_.

Change of this parameter requires restart of the Knot server to take effect.

*Default:* ``none``

.. _server_dbus-init-delay:

dbus-init-delay
---------------

Time in seconds which the server waits upon D-Bus initialization to ensure
the D-Bus client is ready to receive signals.

Change of this parameter requires restart of the Knot server to take effect.

*Minimum:* ``0``

*Default:* ``1``

.. _server_listen:

listen
------

One or more IP addresses where the server listens for incoming queries.
Optional port specification (default is 53) can be appended to each address
using ``@`` separator. Use ``0.0.0.0`` for all configured IPv4 addresses or
``::`` for all configured IPv6 addresses. Filesystem path can be specified
for listening on local unix SOCK_STREAM socket. Non-absolute path
(i.e. not starting with ``/``) is relative to :ref:`server_rundir`.
Non-local address binding is automatically enabled if supported by the operating system.

Change of this parameter requires restart of the Knot server to take effect.

*Default:* not set

.. _server_listen-quic:

listen-quic
-----------

One or more IP addresses (and optionally ports) where the server listens
for incoming queries over QUIC protocol.

Change of this parameter requires restart of the Knot server to take effect.

*Default:* not set

.. _server_listen-tls:

listen-tls
----------

One or more IP addresses (and optionally ports) where the server listens
for incoming queries over TLS protocol (DoT).

Change of this parameter requires restart of the Knot server to take effect.

*Default:* not set

.. _xdp section:

``xdp`` section
===============

Various options related to XDP listening, especially TCP.

::

 xdp:
     listen: STR[@INT] | ADDR[@INT] ...
     udp: BOOL
     tcp: BOOL
     quic: BOOL
     quic-port: INT
     tcp-max-clients: INT
     tcp-inbuf-max-size: SIZE
     tcp-outbuf-max-size: SIZE
     tcp-idle-close-timeout: TIME
     tcp-idle-reset-timeout: TIME
     tcp-resend-timeout: TIME
     route-check: BOOL
     ring-size: INT
     busypoll-budget: INT
     busypoll-timeout: INT

.. CAUTION::
   When you change configuration parameters dynamically or via configuration file
   reload, some parameters in the XDP section require restarting the Knot server
   so that the changes take effect.

.. _xdp_listen:

listen
------

One or more network device names (e.g. ``ens786f0``) on which the :ref:`Mode XDP`
is enabled. Alternatively, an IP address can be used instead of a device name,
but the server will still listen on all addresses belonging to the same interface!
Optional port specification (default is 53) can be appended to each device name
or address using ``@`` separator.

Change of this parameter requires restart of the Knot server to take effect.

.. CAUTION::
   If XDP workers only process regular DNS traffic over UDP, it is strongly
   recommended to also :ref:`listen <server_listen>` on the addresses which are
   intended to offer the DNS service, at least to fulfil the DNS requirement for
   working TCP.

.. NOTE::
   Incoming :ref:`DDNS<dynamic updates>` over XDP isn't supported.
   The server always responds with SERVFAIL.

*Default:* not set

.. _xdp_udp:

udp
---

If enabled, DNS over UDP is processed with XDP workers.

Change of this parameter requires restart of the Knot server to take effect.

*Default:* ``on``

.. _xdp_tcp:

tcp
---

If enabled, DNS over TCP traffic is processed with XDP workers.

The TCP stack limitations:

 - Congestion control is not implemented.
 - Lost packets that do not contain TCP payload may not be resend.
 - Not optimized for transfers of non-trivial zones.

Change of this parameter requires restart of the Knot server to take effect.

*Default:* ``off``

.. _xdp_quic:

quic
----

If enabled, DNS over QUIC is processed with XDP workers.

Change of this parameter requires restart of the Knot server to take effect.

*Default:* ``off``

.. _xdp_quic-port:

quic-port
---------

DNS over QUIC will listen on the interfaces configured by :ref:`xdp_listen`,
but on different port, configured by this option.

Change of this parameter requires restart of the Knot server to take effect.

*Default:* ``853``

.. _xdp_tcp-max-clients:

tcp-max-clients
---------------

A maximum number of TCP clients connected in parallel.

*Minimum:* ``1024``

*Default:* ``1000000`` (one million)

.. _xdp_tcp-inbuf-max-size:

tcp-inbuf-max-size
------------------

Maximum cumulative size of memory used for buffers of incompletely
received messages.

*Minimum:* ``1M`` (1 MiB)

*Default:* ``100M`` (100 MiB)

.. _xdp_tcp-outbuf-max-size:

tcp-outbuf-max-size
-------------------

Maximum cumulative size of memory used for buffers of unACKed
sent messages.

*Minimum:* ``1M`` (1 MiB)

*Default:* ``100M`` (100 MiB)

.. _xdp_tcp-idle-close-timeout:

tcp-idle-close-timeout
----------------------

Time in seconds, after which any idle connection is gracefully closed.

*Minimum:* ``1``

*Default:* ``10``

.. _xdp_tcp-idle-reset-timeout:

tcp-idle-reset-timeout
----------------------

Time in seconds, after which any idle connection is forcibly closed.

*Minimum:* ``1``

*Default:* ``20``

.. _xdp_tcp-resend-timeout:

tcp-resend-timeout
------------------

Resend outgoing data packets (with DNS response payload) if not ACKed
before this timeout (in seconds).

*Minimum:* ``1``

*Default:* ``5``

.. _xdp_route-check:

route-check
-----------

If enabled, routing information from the operating system is considered
when processing every incoming DNS packet received over the XDP interface:

- If the outgoing interface of the corresponding DNS response differs from
  the incoming one, the packet is processed normally by UDP/TCP workers
  (XDP isn't used).
- If the destination address is blackholed, unreachable, or prohibited,
  the DNS packet is dropped without any response.
- The destination MAC address and possible VLAN tag for the response are taken
  from the routing system.

If disabled, symmetrical routing is applied. It means that the query source
MAC address is used as a response destination MAC address. Possible VLAN tag
is preserved.

Change of this parameter requires restart of the Knot server to take effect.

.. NOTE::
   This mode requires forwarding enabled on the loopback interface
   (``sysctl -w net.ipv4.conf.lo.forwarding=1`` and ``sysctl -w net.ipv6.conf.lo.forwarding=1``).
   If forwarding is disabled, all incoming DNS packets are dropped!

   Only VLAN 802.1Q is supported.

*Default:* ``off``

.. _xdp_ring-size:

ring-size
---------

Size of RX, FQ, TX, and CQ rings.

Change of this parameter requires restart of the Knot server to take effect.

.. NOTE::
   This value should be at least as high as the configured RX size of the
   network device in the XDP mode.

*Default:* ``2048``

.. _xdp_busypoll-budget:

busypoll-budget
---------------

If set to a positive value, preferred busy polling is enabled with the
specified budget.

Change of this parameter requires restart of the Knot server to take effect.

.. NOTE::

   Preferred busy polling also requires setting ``napi_defer_hard_irqs`` and
   ``gro_flush_timeout`` for the appropriate network interface. E.g.::

     echo 2 | sudo tee /sys/class/net/<interface>/napi_defer_hard_irqs
     echo 200000 | sudo tee /sys/class/net/<interface>/gro_flush_timeout

.. NOTE::

   A recommended value is between 8 and 64.

*Default:* ``0`` (disabled)

.. _xdp_busypoll-timeout:

busypoll-timeout
----------------

Timeout in microseconds of preferrred busy polling if enabled by
:ref:`xdp_busypoll-budget`.

Change of this parameter requires restart of the Knot server to take effect.

*Default:* ``20`` (20 microseconds)

.. _control section:

``control`` section
===================

Configuration of the server control interface.

::

 control:
     listen: STR
     backlog: INT
     timeout: TIME

.. _control_listen:

listen
------

A UNIX socket :ref:`path<default_paths>` where the server listens for
control commands.

Change of this parameter requires restart of the Knot server to take effect.

*Default:* :ref:`rundir<server_rundir>`\ ``/knot.sock``

.. _control_backlog:

backlog
-------

The control UNIX socket listen backlog size.

Change of this parameter requires restart of the Knot server to take effect.

*Default:* ``5``

.. _control_timeout:

timeout
-------

Maximum time (in seconds) the control socket operations can take.
Set to 0 for infinity.

*Default:* ``5``

.. _log section:

``log`` section
===============

Server can be configured to log to the standard output, standard error
output, syslog (or systemd journal if systemd is enabled) or into an arbitrary
file.

There are 6 logging severity levels:

- ``critical`` – Non-recoverable error resulting in server shutdown.
- ``error`` – Recoverable error, action should be taken.
- ``warning`` – Warning that might require user action.
- ``notice`` – Server notice or hint.
- ``info`` – Informational message.
- ``debug`` – Debug or detailed message.

In the case of a missing log section, ``warning`` or more serious messages
will be logged to both standard error output and syslog. The ``info`` and
``notice`` messages will be logged to standard output.

::

 log:
   - target: stdout | stderr | syslog | STR
     server: critical | error | warning | notice | info | debug
     control: critical | error | warning | notice | info | debug
     zone: critical | error | warning | notice | info | debug
     quic: critical | error | warning | notice | info | debug
     any: critical | error | warning | notice | info | debug

.. _log_target:

target
------

A logging output.

Possible values:

- ``stdout`` – Standard output.
- ``stderr`` – Standard error output.
- ``syslog`` – Syslog or systemd journal.
- *file\_name* – A specific file.

With ``syslog`` target, syslog service is used. However, if Knot DNS has been compiled
with systemd support and operating system has been booted with systemd, systemd journal
is used for logging instead of syslog.

A *file_name* may be specified as an absolute path or a path relative to the
:doc:`knotd<man_knotd>` startup directory.

.. _log_server:

server
------

Minimum severity level for messages related to general operation of the server to be
logged.

*Default:* not set

.. _log_control:

control
-------

Minimum severity level for messages related to server control to be logged.

*Default:* not set

.. _log_zone:

zone
----

Minimum severity level for messages related to zones to be logged.

*Default:* not set

.. _log_quic:

quic
----

Minimum severity level for messages related to QUIC to be logged.

*Default:* not set

.. _log_any:

any
---

Minimum severity level for all message types, except ``quic``, to be logged.

*Default:* not set

.. _stats section:

``statistics`` section
======================

Periodic server statistics dumping.

::

  statistics:
      timer: TIME
      file: STR
      append: BOOL

.. _statistics_timer:

timer
-----

A period (in seconds) after which all available statistics metrics will by written to the
:ref:`file<statistics_file>`.

*Default:* not set

.. _statistics_file:

file
----

A file :ref:`path<default_paths>` of statistics output in the YAML format.

*Default:* :ref:`rundir<server_rundir>`\ ``/stats.yaml``

.. _statistics_append:

append
------

If enabled, the output will be appended to the :ref:`file<statistics_file>`
instead of file replacement.

*Default:* ``off``

.. _database section:

``database`` section
====================

Configuration of databases for zone contents, DNSSEC metadata, or event timers.

::

 database:
     storage: STR
     journal-db: STR
     journal-db-mode: robust | asynchronous
     journal-db-max-size: SIZE
     kasp-db: STR
     kasp-db-max-size: SIZE
     timer-db: STR
     timer-db-max-size: SIZE
     catalog-db: str
     catalog-db-max-size: SIZE

.. _database_storage:

storage
-------

A data directory for storing journal, KASP, and timer databases. A non-absolute
path is relative to the :doc:`knotd<man_knotd>` startup directory.

*Default:* ``${localstatedir}/lib/knot`` (configured with ``--with-storage=path``)

.. _database_journal-db:

journal-db
----------

An explicit :ref:`specification<default_paths>` of the persistent journal database
directory.

*Default:* :ref:`storage<database_storage>`\ ``/journal``

.. _database_journal-db-mode:

journal-db-mode
---------------

Specifies journal LMDB backend configuration, which influences performance
and durability.

Possible values:

- ``robust`` – The journal database disk synchronization ensures database
  durability but is generally slower.
- ``asynchronous`` – The journal database disk synchronization is optimized for
  better performance at the expense of lower database durability in the case of
  a crash. This mode is recommended on secondary servers with many zones.

*Default:* ``robust``

.. _database_journal-db-max-size:

journal-db-max-size
-------------------

The hard limit for the journal database maximum size. There is no cleanup logic
in journal to recover from reaching this limit. Journal simply starts refusing
changes across all zones. Decreasing this value has no effect if it is lower
than the actual database file size.

It is recommended to limit :ref:`journal-max-usage<zone_journal-max-usage>`
per-zone instead of :ref:`journal-db-max-size<database_journal-db-max-size>`
in most cases. Please keep this value larger than the sum of all zones'
journal usage limits. See more details regarding
:ref:`journal behaviour<Journal behaviour>`.

.. NOTE::
   This value also influences server's usage of virtual memory.

*Default:* ``20G`` (20 GiB), or ``512M`` (512 MiB) for 32-bit

.. _database_kasp-db:

kasp-db
-------

An explicit :ref:`specification<default_paths>` of the KASP database directory.

*Default:* :ref:`storage<database_storage>`\ ``/keys``

.. _database_kasp-db-max-size:

kasp-db-max-size
----------------

The hard limit for the KASP database maximum size.

.. NOTE::
   This value also influences server's usage of virtual memory.

*Default:* ``500M`` (500 MiB)

.. _database_timer-db:

timer-db
--------

An explicit :ref:`specification<default_paths>` of the persistent timer
database directory.

*Default:* :ref:`storage<database_storage>`\ ``/timers``

.. _database_timer-db-max-size:

timer-db-max-size
-----------------

The hard limit for the timer database maximum size.

.. NOTE::
   This value also influences server's usage of virtual memory.

*Default:* ``100M`` (100 MiB)

.. _database_catalog-db:

catalog-db
----------

An explicit :ref:`specification<default_paths>` of the zone catalog
database directory. Only useful if :ref:`catalog-zones` are enabled.

*Default:* :ref:`storage<database_storage>`\ ``/catalog``

.. _database_catalog-db-max-size:

catalog-db-max-size
-------------------

The hard limit for the catalog database maximum size.

.. NOTE::
   This value also influences server's usage of virtual memory.

*Default:* ``20G`` (20 GiB), or ``512M`` (512 MiB) for 32-bit

.. _keystore section:

``keystore`` section
====================

DNSSEC keystore configuration.

::

 keystore:
   - id: STR
     backend: pem | pkcs11
     config: STR
     key-label: BOOL

.. _keystore_id:

id
--

A keystore identifier.


.. _keystore_backend:

backend
-------

A key storage backend type.

Possible values:

- ``pem`` – PEM files.
- ``pkcs11`` – PKCS #11 storage.

*Default:* ``pem``

.. _keystore_config:

config
------

A backend specific configuration. A directory with PEM files (the path can
be specified as a relative path to :ref:`kasp-db<database_kasp-db>`) or
a configuration string for PKCS #11 storage (`<pkcs11-uri> <module-path>`).
The PKCS #11 URI Scheme is defined in :rfc:`7512`.

.. NOTE::
   Example configuration string for PKCS #11::

     "pkcs11:token=knot;pin-value=1234 /usr/lib64/pkcs11/libsofthsm2.so"

*Default:* :ref:`kasp-db<database_kasp-db>`\ ``/keys``

.. _keystore_key-label:

key-label
---------

If enabled in combination with the PKCS #11 :ref:`keystore_backend`, generated keys
are labeled in the form ``<zone_name> KSK|ZSK``.

*Default:* ``off``

.. _key section:

``key`` section
===============

Shared TSIG keys used to authenticate communication with the server.

::

 key:
   - id: DNAME
     algorithm: hmac-md5 | hmac-sha1 | hmac-sha224 | hmac-sha256 | hmac-sha384 | hmac-sha512
     secret: BASE64

.. _key_id:

id
--

A key name identifier.

.. NOTE::
   This value MUST be exactly the same as the name of the TSIG key on the
   opposite primary/secondary server(s).

.. _key_algorithm:

algorithm
---------

A TSIG key algorithm. See
`TSIG Algorithm Numbers <https://www.iana.org/assignments/tsig-algorithm-names/tsig-algorithm-names.xhtml>`_.

Possible values:

- ``hmac-md5``
- ``hmac-sha1``
- ``hmac-sha224``
- ``hmac-sha256``
- ``hmac-sha384``
- ``hmac-sha512``

*Default:* ``hmac-sha256``

.. _key_secret:

secret
------

Shared key secret.

*Default:* not set

.. _remote section:

``remote`` section
==================

Definitions of remote servers for outgoing connections (source of a zone
transfer, target for a notification, etc.).

::

 remote:
   - id: STR
     address: ADDR[@INT] | STR ...
     via: ADDR[@INT] ...
     quic: BOOL
     tls: BOOL
     key: key_id
     cert-key: BASE64 ...
     block-notify-after-transfer: BOOL
     no-edns: BOOL
     automatic-acl: BOOL

.. _remote_id:

id
--

A remote identifier.

.. _remote_address:

address
-------

An ordered list of destination IP addresses or UNIX socket paths which are
used for communication with the remote server. Non-absolute path
(i.e. not starting with ``/``) is relative to :ref:`server_rundir`.
Optional destination port (default is 53 for UDP/TCP and 853 for QUIC)
can be appended to the address using ``@`` separator.
The addresses are tried in sequence until the
remote is reached.

*Default:* not set

.. NOTE::
   If the remote is contacted and it refuses to perform requested action,
   no more addresses will be tried for this remote.

.. _remote_via:

via
---

An ordered list of source IP addresses which are used as source addresses
for communication with the remote. For the N-th :ref:`remote address <remote_address>`,
the last, but at most N-th, specified :ref:`via address<remote_via>`
of the same family is used.
This option can help if the server listens on more addresses.
Optional source port (default is random) can be appended
to the address using ``@`` separator.

*Default:* not set

.. NOTE::

  For the following configuration:

  ::

    remote:
      - id: example
        address: [198.51.100.10, 2001:db8::10, 198.51.100.20, 2001:db8::20]
        via: [198.51.100.1, 198.51.100.2, 2001:db8::1]

  the (``via`` -> ``address``) mapping is:

  - ``198.51.100.1`` -> ``198.51.100.10``
  - ``2001:db8::1`` ->  ``2001:db8::10``
  - ``198.51.100.2`` -> ``198.51.100.20``
  - ``2001:db8::1`` -> ``2001:db8::20``

.. _remote_quic:

quic
----

If this option is set, the QUIC protocol will be used for outgoing communication
with this remote.

.. NOTE::
   One connection per each remote is opened; :ref:`server_remote-pool-limit`
   does not take effect for QUIC. However, fast QUIC handshakes utilizing obtained
   session tickets are used for reopening connections to recently (up to 1 day)
   queried remotes.

*Default:* ``off``

.. _remote_tls:

tls
---

If this option is set, the TLS (DoT) protocol will be used for outgoing communication
with this remote.

*Default:* ``off``

.. _remote_key:

key
---

A :ref:`reference<key_id>` to the TSIG key which is used to authenticate
the communication with the remote server.

*Default:* not set

.. _remote_cert-key:

cert-key
--------

An ordered list of remote certificate public key PINs. If the list is non-empty,
communication with the remote is possible only via QUIC or TLS protocols and
a peer certificate is required. The peer certificate key must match one of the
specified PINs.

A PIN is a unique identifier that represents the public key of the peer certificate.
It's a base64-encoded SHA-256 hash of the public key. This identifier
remains the same on a certificate renewal.

*Default:* not set

.. _remote_block-notify-after-transfer:

block-notify-after-transfer
---------------------------

When incoming AXFR/IXFR from this remote (as a primary server), suppress
sending NOTIFY messages to all configured secondary servers.

*Default:* ``off``

.. _remote_no-edns:

no-edns
-------

If enabled, no OPT record (EDNS) is inserted to outgoing requests to this
remote server. This mode is necessary for communication with some broken
implementations (e.g. Windows Server 2016).

.. NOTE::
   This option effectively disables :ref:`zone expire<Zone expiration>` timer
   updates via EDNS EXPIRE option specified in :rfc:`7314`.

*Default:* ``off``

.. _remote_automatic-acl:

automatic-acl
-------------

If enabled, some authorized operations for the remote are automatically allowed
based on the context:

- Incoming NOTIFY is allowed from the remote if it's configured as a
  :ref:`primary server <zone_master>` for the zone.
- Outgoing zone transfer is allowed to the remote if it's configured as a
  :ref:`NOTIFY target <zone_notify>` for the zone.

Automatic ACL rules are evaluated before explicit :ref:`zone ACL <zone_acl>` configuration.

.. NOTE::
   This functionality requires global activation via
   :ref:`server_automatic-acl` in the server section.

*Default:* ``on``

.. _remotes section:

``remotes`` section
===================

Definitions of groups of remote servers. Remote grouping can simplify the
configuration.

::

 remotes:
   - id: STR
     remote: remote_id ...

.. _remotes_id:

id
--

A remote group identifier.

.. _remotes_remote:

remote
------

An ordered list of :ref:`references<remote_id>` to remote server definitions.

*Default:* not set

.. _acl section:

``acl`` section
===============

Access control list rule definitions. An ACL rule is a description of one
or more authorized actions (zone transfer request, zone change notification,
and dynamic DNS update) which are allowed to be processed or denied. Queries
which don't require authorization are always allowed.

::

 acl:
   - id: STR
     address: ADDR[/INT] | ADDR-ADDR | STR ...
     key: key_id ...
     cert-key: BASE64 ...
     remote: remote_id | remotes_id ...
     action: query | notify | transfer | update ...
     protocol: udp | tcp | tls | quic ...
     deny: BOOL
     update-type: STR ...
     update-owner: key | zone | name
     update-owner-match: sub-or-equal | equal | sub | pattern
     update-owner-name: STR ...

.. _acl_id:

id
--

An ACL rule identifier.

.. _acl_address:

address
-------

An ordered list of IP addresses, absolute UNIX socket paths, network subnets,
or network ranges. The query's
source address must match one of them. If this item is not set, address match is not
required.

*Default:* not set

.. _acl_key:

key
---

An ordered list of :ref:`reference<key_id>`\ s to TSIG keys. The query must
match one of them. If this item is not set, transaction authentication is not used.

*Default:* not set

.. _acl_cert-key:

cert-key
--------

An ordered list of remote certificate public key PINs. If the list is non-empty,
communication with the remote is possible only via QUIC or TLS protocols and
a peer certificate is required. The peer certificate key must match one of the
specified PINs.

A PIN is a unique identifier that represents the public key of the peer certificate.
It's a base64-encoded SHA-256 hash of the public key. This identifier
remains the same on a certificate renewal.

*Default:* not set

.. _acl_remote:

remote
------

An ordered list of references :ref:`remote<remote_id>` and
:ref:`remotes<remotes_id>`. The query must
match one of the remotes. Specifically, one of the remote's addresses and remote's
TSIG key if configured must match.

.. NOTE::
   This option cannot be specified along with the :ref:`acl_address`,
   :ref:`acl_key`, or :ref:`acl_protocol` option at one ACL item.

*Default:* not set

.. _acl_action:

action
------

An ordered list of allowed, or denied, actions (request types).

Possible values:

- ``query`` – Allow regular DNS query. As normal queries are always allowed,
  this action is only useful in combination with :ref:`TSIG key<acl_key>`.
- ``notify`` – Allow incoming notify (NOTIFY).
- ``transfer`` – Allow zone transfer (AXFR, IXFR).
- ``update`` – Allow zone updates (DDNS).

*Default:* ``query``

.. _acl_protocol:

protocol
--------

List of allowed protocols.

Possible values:

- ``udp`` – UDP protocol.
- ``tcp`` – TCP protocol.
- ``tls`` – TLS protocol.
- ``quic`` – QUIC protocol.

*Default:* not set (any)

.. _acl_deny:

deny
----

If enabled, instead of allowing, deny the matching combination of the specified items.

*Default:* ``off``

.. _acl_update-type:

update-type
-----------

A list of allowed types of Resource Records in a zone update. Every record in an update
must match one of the specified types.

*Default:* not set

.. _acl_update-owner:

update-owner
------------

This option restricts possible owners of Resource Records in a zone update by comparing
them to either the :ref:`TSIG key<acl_key>` identity, the current zone name, or to a list of
domain names given by the :ref:`acl_update-owner-name` option.
The comparison method is given by the :ref:`acl_update-owner-match` option.

Possible values:

- ``key`` — The owner of each updated RR must match the identity of the TSIG key if used.
- ``name`` — The owner of each updated RR must match at least one name in the
  :ref:`acl_update-owner-name` list.
- ``zone`` — The owner of each updated RR must match the current zone name.

*Default:* not set

.. _acl_update-owner-match:

update-owner-match
------------------

This option defines how the owners of Resource Records in an update are matched to the domain name(s)
set by the :ref:`acl_update-owner` option.

Possible values:

- ``sub-or-equal`` — The owner of each RR in an update must either be equal to
  or be a subdomain of at least one domain name set by :ref:`acl_update-owner`.
- ``equal`` — The owner of each updated RR must be equal to at least one domain
  name set by :ref:`acl_update-owner`.
- ``sub`` — The owner of each updated RR must be a subdomain of, but MUST NOT
  be equal to at least one domain name set by :ref:`acl_update-owner`.
- ``pattern`` — The owner of each updated RR must match a pattern specified by
  :ref:`acl_update-owner`. The pattern can be an arbitrary FQDN or non-FQDN
  domain name. If a label consists of one ``*`` (asterisk) character, it
  matches any label. More asterisk labels can be specified.

*Default:* ``sub-or-equal``

.. _acl_update-owner-name:

update-owner-name
-----------------

A list of allowed owners of RRs in a zone update used with :ref:`acl_update-owner`
set to ``name``. Every listed owner name which is not FQDN (i.e. it doesn't end
in a dot) is considered as if it was appended with the target zone name.
Such a relative owner name specification allows better ACL rule reusability across
multiple zones.

*Default:* not set

.. _submission section:

``submission`` section
======================

Parameters of KSK submission checks.

::

 submission:
   - id: STR
     parent: remote_id | remotes_id ...
     check-interval: TIME
     timeout: TIME
     parent-delay: TIME

.. _submission_id:

id
--

A submission identifier.

.. _submission_parent:

parent
------

A list of references :ref:`remote<remote_id>` and :ref:`remotes<remotes_id>`
to parent's DNS servers to be checked for
presence of corresponding DS records in the case of KSK submission. All of them must
have a corresponding DS for the rollover to continue. If none is specified, the
rollover must be pushed forward manually.

*Default:* not set

.. TIP::
   A DNSSEC-validating resolver can be set as a parent.

.. _submission_check-interval:

check-interval
--------------

Interval (in seconds) for periodic checks of DS presence on parent's DNS
servers, in the case of the KSK submission.

*Default:* ``1h`` (1 hour)

.. _submission_timeout:

timeout
-------

After this time period (in seconds) the KSK submission is automatically considered
successful, even if all the checks were negative or no parents are configured.
Set to 0 for infinity.

*Default:* ``0``

.. _submission_parent-delay:

parent-delay
------------

After successful parent DS check, wait for this period (in seconds) before
continuing the next key roll-over step. This delay shall cover the propagation
delay of update in the parent zone.

*Default:* ``0``

.. _dnskey-sync section:

``dnskey-sync`` section
=======================

Parameters of DNSKEY dynamic-update synchronization.

::

 dnskey-sync:
   - id: STR
     remote: remote_id | remotes_id ...
     check-interval: TIME

.. _dnskey-sync_id:

id
--

A dnskey-sync identifier.

.. _dnskey-sync_remote:

remote
------

A list of references :ref:`remote<remote_id>` and :ref:`remotes<remotes_id>`
to other signers or common master, which the DDNS updates with
DNSKEY/CDNSKEY/CDS records shall be sent to.

*Default:* not set

.. _dnskey-sync_check-interval:

check-interval
--------------

If the last DNSKEY sync failed or resulted in any change, re-check
the consistence after this interval (in seconds) and re-try if needed.

*Default:* ``60`` (1 minute)

.. _policy section:

``policy`` section
==================

DNSSEC policy configuration.

::

 policy:
   - id: STR
     keystore: keystore_id
     manual: BOOL
     single-type-signing: BOOL
     algorithm: rsasha1 | rsasha1-nsec3-sha1 | rsasha256 | rsasha512 | ecdsap256sha256 | ecdsap384sha384 | ed25519 | ed448
     ksk-size: SIZE
     zsk-size: SIZE
     ksk-shared: BOOL
     dnskey-ttl: TIME
     zone-max-ttl: TIME
     keytag-modulo: INT/INT
     ksk-lifetime: TIME
     zsk-lifetime: TIME
     delete-delay: TIME
     propagation-delay: TIME
     rrsig-lifetime: TIME
     rrsig-refresh: TIME
     rrsig-pre-refresh: TIME
     reproducible-signing: BOOL
     nsec3: BOOL
     nsec3-iterations: INT
     nsec3-opt-out: BOOL
     nsec3-salt-length: INT
     nsec3-salt-lifetime: TIME
     signing-threads: INT
     ksk-submission: submission_id
     ds-push: remote_id | remotes_id ...
     cds-cdnskey-publish: none | delete-dnssec | rollover | always | double-ds
     cds-digest-type: sha256 | sha384
     dnskey-management: full | incremental
     offline-ksk: BOOL
     unsafe-operation: none | no-check-keyset | no-update-dnskey | no-update-nsec | no-update-expired ...

.. _policy_id:

id
--

A policy identifier.

.. _policy_keystore:

keystore
--------

A :ref:`reference<keystore_id>` to a keystore holding private key material
for zones.

*Default:* an imaginary keystore with all default values

.. NOTE::
   A configured keystore called "default" won't be used unless explicitly referenced.

.. _policy_manual:

manual
------

If enabled, automatic key management is not used.

*Default:* ``off``

.. _policy_single-type-signing:

single-type-signing
-------------------

If enabled, Single-Type Signing Scheme is used in the automatic key management
mode.

*Default:* ``off`` (:ref:`module onlinesign<mod-onlinesign>` has default ``on``)

.. _policy_algorithm:

algorithm
---------

An algorithm of signing keys and issued signatures. See
`DNSSEC Algorithm Numbers <https://www.iana.org/assignments/dns-sec-alg-numbers/dns-sec-alg-numbers.xhtml#dns-sec-alg-numbers-1>`_.

Possible values:

- ``rsasha1``
- ``rsasha1-nsec3-sha1``
- ``rsasha256``
- ``rsasha512``
- ``ecdsap256sha256``
- ``ecdsap384sha384``
- ``ed25519``
- ``ed448``

.. NOTE::
   Ed448 algorithm is only available if compiled with GnuTLS 3.6.12+ and Nettle 3.6+.

*Default:* ``ecdsap256sha256``

.. _policy_ksk-size:

ksk-size
--------

A length of newly generated :abbr:`KSK (Key Signing Key)` or
:abbr:`CSK (Combined Signing Key)` keys.

*Default:* ``2048`` (rsa*), ``256`` (ecdsap256), ``384`` (ecdsap384), ``256`` (ed25519),
``456`` (ed448)

.. _policy_zsk-size:

zsk-size
--------

A length of newly generated :abbr:`ZSK (Zone Signing Key)` keys.

*Default:* see default for :ref:`ksk-size<policy_ksk-size>`

.. _policy_ksk-shared:

ksk-shared
----------

If enabled, all zones with this policy assigned will share one or more KSKs.
More KSKs can be shared during a KSK rollover.

.. WARNING::
   As the shared KSK set is bound to the policy :ref:`id<policy_id>`, renaming the
   policy breaks this connection and new shared KSK set is initiated when
   a new KSK is needed.

*Default:* ``off``

.. _policy_dnskey-ttl:

dnskey-ttl
----------

A TTL value for DNSKEY records added into zone apex.

.. NOTE::
   Has influence over ZSK key lifetime.

.. WARNING::
   Ensure all DNSKEYs with updated TTL are propagated before any subsequent
   DNSKEY rollover starts.

*Default:* zone SOA TTL

.. _policy_zone-max-ttl:

zone-max-ttl
------------

Declare (override) maximal TTL value among all the records in zone.

.. NOTE::
   It's generally recommended to override the maximal TTL computation by setting this
   explicitly whenever possible. It's required for :ref:`DNSSEC Offline KSK` and
   really reasonable when records are generated dynamically
   (e.g. by a :ref:`module<mod-synthrecord>`).

*Default:* computed after zone is loaded

.. _policy_keytag-modulo:

keytag-modulo
-------------

Specifies that the keytags of any generated keys shall be congruent by specified modulo.
The option value must be a string in the format ``R/M``, where ``R < M <= 256`` are
positive integers. Whenever a DNSSEC key is generated, it is ensured
that ``keytag % M == R``. This prevents keytag conflict in :ref:`DNSSEC Offline KSK`
or :ref:`DNSSEC multi-signer` (and possibly other) setups.

.. NOTE::
   This only applies to newly generated keys when they are generated. Keys from
   before this option and keys imported from elsewhere might not fulfill the policy.

*Default:* ``0/1``

.. _policy_ksk-lifetime:

ksk-lifetime
------------

A period (in seconds) between KSK generation and the next rollover initiation.

.. NOTE::
   KSK key lifetime is also influenced by propagation-delay, dnskey-ttl,
   and KSK submission delay.

   Zero (aka infinity) value causes no KSK rollover as a result.

   This applies for CSK lifetime if single-type-signing is enabled.

*Default:* ``0`` (infinity)

.. _policy_zsk-lifetime:

zsk-lifetime
------------

A period (in seconds) between ZSK activation and the next rollover initiation.

.. NOTE::
   More exactly, this period is measured since a ZSK is activated,
   and after this, a new ZSK is generated to replace it within
   following roll-over.

   As a consequence, in normal operation, this results in the period
   of ZSK generation being `zsk-lifetime + propagation-delay + dnskey_ttl`.

   Zero (aka infinity) value causes no ZSK rollover as a result.

*Default:* ``30d`` (30 days)

.. _policy_delete-delay:

delete-delay
------------

Once a key (KSK or ZSK) is rolled-over and removed from the zone,
keep it in the KASP database for at least this period (in seconds) before deleting
it completely. This might be useful in some troubleshooting cases when resurrection
is needed.

*Default:* ``0``

.. _policy_propagation-delay:

propagation-delay
-----------------

An extra delay added for each key rollover step. This value (in seconds)
should be high enough to cover propagation of data from the primary server
to all secondary servers, as well as the duration of signing routine itself
and possible outages in signing and propagation infrastructure. In other
words, this delay should ensure that within this period of time after
planned change of the key set, all public-facing secondaries will already
serve new DNSKEY RRSet for sure.

.. NOTE::
   Has influence over ZSK key lifetime.

*Default:* ``1h`` (1 hour)

.. _policy_rrsig-lifetime:

rrsig-lifetime
--------------

A validity period (in seconds) of newly issued signatures.

.. NOTE::
   The RRSIG's signature inception time is set to 90 minutes in the past. This
   time period is not counted to the signature lifetime.

*Default:* ``14d`` (14 days)

.. _policy_rrsig-refresh:

rrsig-refresh
-------------

A period (in seconds) how long at least before a signature expiration the signature
will be refreshed, in order to prevent expired RRSIGs on secondary servers or
resolvers' caches.

*Default:* 0.1 * :ref:`policy_rrsig-lifetime` + :ref:`policy_propagation-delay` + :ref:`policy_zone-max-ttl`

If :ref:`zone_dnssec-validation` is enabled:

*Default:* ``1d`` (1 day)

.. _policy_rrsig-pre-refresh:

rrsig-pre-refresh
-----------------

A period (in seconds) how long at most before a signature refresh time the signature
might be refreshed, in order to refresh RRSIGs in bigger batches on a frequently updated
zone (avoid re-sign event too often).

*Default:* ``1h`` (1 hour)

.. _policy_reproducible-signing:

reproducible-signing
--------------------

For ECDSA algorithms, generate RRSIG signatures deterministically (:rfc:`6979`).
Besides better theoretical cryptographic security, this mode allows significant
speed-up of loading signed (by the same method) zones. However, the zone signing
is a bit slower.

*Default:* ``off``

.. _policy_nsec3:

nsec3
-----

Specifies if NSEC3 will be used instead of NSEC.

*Default:* ``off``

.. _policy_nsec3-iterations:

nsec3-iterations
----------------

A number of additional times the hashing is performed.

*Default:* ``0``

.. _policy_nsec3-opt-out:

nsec3-opt-out
-------------

If set, NSEC3 records won't be created for insecure delegations.
This speeds up the zone signing and reduces overall zone size.

.. WARNING::
  NSEC3 with the Opt-Out bit set no longer works as a proof of non-existence
  in this zone.

*Default:* ``off``

.. _policy_nsec3-salt-length:

nsec3-salt-length
-----------------

A length of a salt field in octets, which is appended to the original owner
name before hashing.

*Default:* ``8``

.. _policy_nsec3-salt-lifetime:

nsec3-salt-lifetime
-------------------

A validity period (in seconds) of newly issued salt field.

Zero value means infinity.

Special value *-1* triggers re-salt every time when active ZSK changes.
This optimizes the number of big changes to the zone.

*Default:* ``30d`` (30 days)

.. _policy_signing-threads:

signing-threads
---------------

When signing zone or update, use this number of threads for parallel signing.

Those are extra threads independent of :ref:`Background workers<server_background-workers>`.

.. NOTE::
   Some steps of the DNSSEC signing operation are not parallelized.

*Default:* ``1`` (no extra threads)

.. _policy_ksk-submission-check:

ksk-submission
--------------

A reference to :ref:`submission<submission_id>` section holding parameters of
KSK submission checks.

*Default:* not set

.. _policy_ds-push:

ds-push
-------

Optional references :ref:`remote<remote_id>` and :ref:`remotes<remotes_id>`
to authoritative DNS server of the
parent's zone. The remote server must be configured to accept DS record
updates via DDNS. Whenever a CDS record in the local zone is changed, the
corresponding DS record is sent as a dynamic update (DDNS) to the parent
DNS server. All previous DS records are deleted within the DDNS message.
It's possible to manage both child and parent zones by the same Knot DNS server.

.. NOTE::
   This feature requires :ref:`cds-cdnskey-publish<policy_cds-cdnskey-publish>`
   not to be set to ``none``.

.. NOTE::
   The mentioned change to CDS record usually means that a KSK roll-over is running
   and the new key being rolled-in is in "ready" state already for the period of
   :ref:`propagation-delay<policy_propagation-delay>`.

.. NOTE::
   Module :ref:`Onlinesign<mod-onlinesign>` doesn't support DS push.

.. NOTE::
   When turning this feature on while a KSK roll-over is already running, it might
   not take effect for the already-running roll-over.

*Default:* not set

.. _policy_dnskey-sync:

dnskey-sync
-----------

A reference to :ref:`dnskey-sync<dnskey-sync_id>` section holding parameters
of DNSKEY synchronization.

*Default:* not set

.. _policy_cds-cdnskey-publish:

cds-cdnskey-publish
-------------------

Controls if and how shall the CDS and CDNSKEY be published in the zone.

Possible values:

- ``none`` – Never publish any CDS or CDNSKEY records in the zone.
- ``delete-dnssec`` – Publish special CDS and CDNSKEY records indicating turning off DNSSEC.
- ``rollover`` – Publish CDS and CDNSKEY records for ready and not yet active KSK (submission phase of KSK rollover).
- ``always`` – Always publish one CDS and one CDNSKEY records for the current KSK.
- ``double-ds`` – Always publish up to two CDS and two CDNSKEY records for ready and/or active KSKs.

.. NOTE::
   If the zone keys are managed manually, the CDS and CDNSKEY rrsets may contain
   more records depending on the keys available.

.. WARNING::
   The ``double-ds`` value does not trigger double-DS roll-over method. That method is
   only supported when performed manually, with unset :ref:`policy_ksk-submission-check`.

*Default:* ``rollover``

.. _policy_cds-digest-type:

cds-digest-type
---------------

Specify digest type for published CDS records.

Possible values:

- ``sha256``
- ``sha384``

*Default:* ``sha256``

.. _policy_dnskey-management:

dnskey-management
-----------------

Specify how the DNSKEY, CDNSKEY, and CDS RRSets at the zone apex are handled
when (re-)signing the zone.

Possible values:

- ``full`` – Upon every zone (re-)sign, delete all unknown DNSKEY, CDNSKEY, and CDS
  records and keep just those that are related to the zone keys stored in the KASP database.
- ``incremental`` – Keep unknown DNSKEY, CDNSKEY, and CDS records in the zone, and
  modify server-managed records incrementally by employing changes in the KASP database.

.. NOTE::
   Prerequisites for *incremental*:

   - The :ref:`Offline KSK <DNSSEC Offline KSK>` isn't supported.
   - The :ref:`policy_delete-delay` is long enough to cover possible daemon
     shutdown (e.g. due to server maintenance).
   - Avoided manual deletion of keys with :doc:`keymgr<man_keymgr>`.

   Otherwise there might remain some DNSKEY records in the zone, belonging to
   deleted keys.

*Default:* ``full``

.. _policy_offline-ksk:

offline-ksk
-----------

Specifies if :ref:`Offline KSK <DNSSEC Offline KSK>` feature is enabled.

*Default:* ``off``

.. _policy_unsafe-operation:

unsafe-operation
----------------

Turn off some DNSSEC safety features.

Possible values:

- ``none`` – Nothing disabled.
- ``no-check-keyset`` – Don't check active keys in present algorithms. This may
  lead to violation of :rfc:`4035#section-2.2`.
- ``no-update-dnskey`` – Don't maintain/update DNSKEY, CDNSKEY, and CDS records
  in the zone apex according to KASP database. Juste leave them as they are in the zone.
- ``no-update-nsec`` – Don't maintain/update NSEC/NSEC3 chain. Leave all the records
  as they are in the zone.
- ``no-update-expired`` – Don't update expired RRSIGs.

Multiple values may be specified.

.. WARNING::
   This mode is intended for DNSSEC experts who understand the corresponding consequences.

*Default:* ``none``

.. _template section:

``template`` section
====================

A template is shareable zone settings, which can simplify configuration by
reducing duplicates. A special default template (with the *default* identifier)
can be used for global zone configuration or as an implicit configuration
if a zone doesn't have another template specified.

::

 template:
   - id: STR
     global-module: STR/STR ...
     # All zone options (excluding 'template' item)

.. NOTE::
   If an item is explicitly specified both in the referenced template and
   the zone, the template item value is overridden by the zone item value.

.. _template_id:

id
--

A template identifier.

.. _template_global-module:

global-module
-------------

An ordered list of references to query modules in the form of *module_name* or
*module_name/module_id*. These modules apply to all queries.

.. NOTE::
   This option is only available in the *default* template.

*Default:* not set

.. _zone section:

``zone`` section
================

Definition of zones served by the server.

::

 zone:
   - domain: DNAME
     template: template_id
     storage: STR
     file: STR
     master: remote_id | remotes_id ...
     ddns-master: remote_id
     notify: remote_id | remotes_id ...
     notify-delay: TIME
     acl: acl_id ...
     master-pin-tolerance: TIME
     provide-ixfr: BOOL
     semantic-checks: BOOL | soft
     default-ttl: TIME
     zonefile-sync: TIME
     zonefile-load: none | difference | difference-no-serial | whole
     journal-content: none | changes | all
     journal-max-usage: SIZE
     journal-max-depth: INT
     ixfr-benevolent: BOOL
     ixfr-by-one: BOOL
     ixfr-from-axfr: BOOL
     zone-max-size : SIZE
     adjust-threads: INT
     dnssec-signing: BOOL
     dnssec-validation: BOOL
     dnssec-policy: policy_id
     ds-push: remote_id | remotes_id ...
     zonemd-verify: BOOL
     zonemd-generate: none | zonemd-sha384 | zonemd-sha512 | remove
     serial-policy: increment | unixtime | dateserial
     serial-modulo: INT/INT | +INT | -INT | INT/INT+INT | INT/INT-INT
     reverse-generate: DNAME ...
     refresh-min-interval: TIME
     refresh-max-interval: TIME
     retry-min-interval: TIME
     retry-max-interval: TIME
     expire-min-interval: TIME
     expire-max-interval: TIME
     catalog-role: none | interpret | generate | member
     catalog-template: template_id ...
     catalog-zone: DNAME
     catalog-group: STR
     module: STR/STR ...

.. _zone_domain:

domain
------

A zone name identifier.

.. _zone_template:

template
--------

A :ref:`reference<template_id>` to a configuration template.

*Default:* not set or ``default`` (if the template exists)

.. _zone_storage:

storage
-------

A data directory for storing zone files. A non-absolute path is relative to
the :doc:`knotd<man_knotd>` startup directory.

*Default:* ``${localstatedir}/lib/knot`` (configured with ``--with-storage=path``)

.. _zone_file:

file
----

A :ref:`path<default_paths>` to the zone file. It is also possible to use
the following formatters:

- ``%c[``\ *N*\ ``]`` or ``%c[``\ *N*\ ``-``\ *M*\ ``]`` – Means the *N*\ th
  character or a sequence of characters beginning from the *N*\ th and ending
  with the *M*\ th character of the textual zone name (see ``%s``). The
  indexes are counted from 0 from the left. All dots (including the terminal
  one) are considered. If the character is not available, the formatter has no effect.
- ``%l[``\ *N*\ ``]`` – Means the *N*\ th label of the textual zone name
  (see ``%s``). The index is counted from 0 from the right (0 ~ TLD).
  If the label is not available, the formatter has no effect.
- ``%s`` – Means the current zone name in the textual representation.
  The zone name doesn't include the terminating dot (the result for the root
  zone is the empty string!).
- ``%%`` – Means the ``%`` character.

.. WARNING::
  Beware of special characters which are escaped or encoded in the \\DDD form
  where DDD is corresponding decimal ASCII code.

*Default:* :ref:`storage<zone_storage>`\ ``/%s.zone``

.. _zone_master:

master
------

An ordered list of references :ref:`remote<remote_id>` and
:ref:`remotes<remotes_id>` to zone primary servers
(formerly known as master servers).
Empty value is allowed for template value overriding.

*Default:* not set

.. _zone_ddns-master:

ddns-master
-----------

A :ref:`reference<remote_id>` to a zone primary master where DDNS messages
should be forwarded to. If not specified, the first :ref:`master<zone_master>`
server is used.

If set to the empty value (""), incoming DDNS messages aren't forwarded but are applied
to the local zone instead, no matter if it is a secondary server. This is only allowed in
combination with :ref:`zone_dnssec-signing` enabled.

*Default:* not set

.. _zone_notify:

notify
------

An ordered list of references :ref:`remote<remote_id>` and
:ref:`remotes<remotes_id>` to secondary servers to which NOTIFY
message is sent if the zone changes.
Empty value is allowed for template value overriding.

*Default:* not set

.. _zone_notify-delay:

notify-delay
------------

The time delay in seconds before an outgoing NOTIFY message is sent upon loading
a new zone (e.g. to ensure that secondaries have enough time to adjust their catalogues).
Set to -1 to prevent sending NOTIFY messages in this context.

*Default:* ``0``

.. _zone_acl:

acl
---

An ordered list of :ref:`references<acl_id>` to ACL rules which can allow
or disallow zone transfers, updates or incoming notifies.

*Default:* not set

.. _zone_master-pin-tolerance:

master-pin-tolerance
--------------------

If set to a nonzero value on a secondary, always request AXFR/IXFR from the same
primary as the last time, effectively pinning one primary. Only when another
primary is updated and the current one lags behind for the specified amount of time
(defined by this option in seconds), change to the updated primary and force AXFR.

This option is useful when multiple primaries may have different zone history
in their journals, making it unsafe to combine interchanged IXFR
from different primaries.

*Default:* ``0`` (disabled)

.. _zone_provide-ixfr:

provide-ixfr
------------

If disabled, the server is forced to respond with AXFR to IXFR queries.
If enabled, IXFR requests are responded normally.

*Default:* ``on``

.. _zone_semantic-checks:

semantic-checks
---------------

Selects if extra zone semantic checks are used or impacts of the mandatory checks.

There are several mandatory checks which are always enabled and cannot be turned
off. An error in a mandatory check causes the zone not to be loaded. Most of
the mandatory checks can be weakened by setting ``soft``, which allows the zone to
be loaded even if the check fails.

If enabled, extra checks are used. These checks don't prevent the zone from loading.

The mandatory checks are applied to zone files, zone transfers, and updates via
control interface. The extra checks are applied to zone files only!

Mandatory checks:

- Missing SOA record at the zone apex (:rfc:`1034`) (*)
- An extra record exists together with a CNAME record except for RRSIG and NSEC (:rfc:`1034`)
- Multiple CNAME records with the same owner exist (:rfc:`1034`)
- DNAME record having a record under it (:rfc:`6672`)
- Multiple DNAME records with the same owner exist (:rfc:`6672`)
- NS record exists together with a DNAME record (:rfc:`6672`)
- DS record exists at the zone apex (:rfc:`3658`)

(*) The marked check can't be weakened by the soft mode. All other mandatory checks
are subject to the optional soft mode.

Extra checks:

- Missing NS record at the zone apex
- Missing glue A or AAAA record
- Invalid DS or NSEC3PARAM record
- CDS or CDNSKEY inconsistency
- All other DNSSEC checks executed during :ref:`zone_dnssec-validation`

.. NOTE::
   The soft mode allows the refresh event to ignore a CNAME response to a SOA
   query (malformed message) and triggers a zone bootstrap instead.

*Default:* ``off``

.. _zone_default-ttl:

default-ttl
-----------

The default TTL value if none is specified in a zone file or zone insertion
using the dynamic configuration.

.. WARNING::
   As changing this value can result in differently parsed zone file(s),
   the corresponding zone SOA serial(s) should be incremented before
   reloading or committing the configuration. Alternatively, setting
   :ref:`zonefile-load <zone_zonefile-load>` to ``difference-no-serial`` ensures
   the resulting zone(s) update is correct.

*Default:* ``3600``

.. _zone_zonefile-sync:

zonefile-sync
-------------

The time in seconds after which the current zone in memory will be synced with
a zone file on the disk (see :ref:`file<zone_file>`). The server will serve the latest
zone even after a restart using zone journal, but the zone file on the disk will
only be synced after ``zonefile-sync`` time has expired (or after manual zone
flush). This is applicable when the zone is updated via IXFR, DDNS or automatic
DNSSEC signing. In order to completely disable automatic zone file synchronization,
set the value to -1. In that case, it is still possible to force a manual zone flush
using the ``-f`` option.

.. NOTE::
   If you are serving large zones with frequent updates where
   the immediate sync with a zone file is not desirable, increase the value.

*Default:* ``0`` (immediate)

.. _zone_zonefile-load:

zonefile-load
-------------

Selects how the zone file contents are applied during zone load.

Possible values:

- ``none`` – The zone file is not used at all.
- ``difference`` – If the zone contents are already available during server start or reload,
  the difference is computed between them and the contents of the zone file. This difference
  is then checked for semantic errors and applied to the current zone contents.
- ``difference-no-serial`` – Same as ``difference``, but the SOA serial in the zone file is
  ignored, the server takes care of incrementing the serial automatically.
- ``whole`` – Zone contents are loaded from the zone file.

When ``difference`` is configured and there are no zone contents yet (cold start
and no zone contents in the journal), it behaves the same way as ``whole``.

*Default:* ``whole``

.. NOTE::
   See :ref:`Handling, zone file, journal, changes, serials` for guidance on
   configuring these and related options to ensure reliable operation.

.. _zone_journal-content:

journal-content
---------------

Selects how the journal shall be used to store zone and its changes.

Possible values:

- ``none`` – The journal is not used at all.
- ``changes`` – Zone changes history is stored in journal.
- ``all`` – Zone contents and history is stored in journal.

*Default:* ``changes``

.. WARNING::
   When this option is changed, the journal still contains data respective to
   the previous setting. For example, changing it to ``none`` does not purge
   the journal. Also, changing it from ``all`` to ``changes``
   does not cause the deletion of the zone-in-journal and the behaviour of the
   zone loading procedure might be different than expected. It is recommended
   to consider purging the journal when this option is changed.

.. _zone_journal-max-usage:

journal-max-usage
-----------------

Policy how much space in journal DB will the zone's journal occupy.

.. NOTE::
   Journal DB may grow far above the sum of journal-max-usage across
   all zones, because of DB free space fragmentation.

*Default:* ``100M`` (100 MiB)

.. _zone_journal-max-depth:

journal-max-depth
-----------------

Maximum history length of the journal.

.. NOTE::
   Zone-in-journal changeset isn't counted to the limit.

*Minimum:* ``2``

*Default:* ``20``

.. _zone_ixfr-benevolent:

ixfr-benevolent
---------------

If enabled, incoming IXFR is applied even when it contains removals of non-existing
or additions of existing records.

*Default:* ``off``

.. _zone_ixfr-by-one:

ixfr-by-one
-----------

Within incoming IXFR, process only one changeset at a time, not multiple together.
This preserves the complete history in the journal and prevents the merging of
changesets when multiple changesets are IXFRed simultaneously. However, this does not
prevent the merging (or deletion) of old changesets in the journal to save space,
as described in :ref:`journal behaviour <Journal behaviour>`.

This option leads to increased server load when processing IXFR, including
network traffic.

*Default:* ``off``

.. _zone_ixfr-from-axfr:

ixfr-from-axfr
--------------

If a primary sends AXFR-style-IXFR upon an IXFR request, compute the difference
and process it as an incremental zone update (e.g. by storing the changeset in
the journal).

*Default:* ``off``

.. _zone_zone-max-size:

zone-max-size
-------------

Maximum size of the zone. The size is measured as size of the zone records
in wire format without compression. The limit is enforced for incoming zone
transfers and dynamic updates.

For incremental transfers (IXFR), the effective limit for the total size of
the records in the transfer is twice the configured value. However the final
size of the zone must satisfy the configured value.

*Default:* unlimited

.. _zone_adjust-threads:

adjust-threads
--------------

Parallelize internal zone adjusting procedures by using specified number of
threads. This is useful with huge zones with NSEC3. Speedup observable at
server startup and while processing NSEC3 re-salt.

*Default:* ``1`` (no extra threads)

.. _zone_dnssec-signing:

dnssec-signing
--------------

If enabled, automatic DNSSEC signing for the zone is turned on.

*Default:* ``off``

.. _zone_dnssec-validation:

dnssec-validation
-----------------

If enabled, the zone contents are validated for being correctly signed
(including NSEC/NSEC3 chain) with DNSSEC signatures every time the zone
is loaded or changed (including AXFR/IXFR).

When the validation fails, the zone being loaded or update being applied
is cancelled with an error, and either none or previous zone state is published.

List of DNSSEC checks:

- Every zone RRSet is correctly signed by at least one present DNSKEY.
- For every RRSIG there are at most 3 non-matching DNSKEYs with the same keytag.
- DNSKEY RRSet is signed by KSK.
- NSEC(3) RR exists for each name (unless opt-out) with correct bitmap.
- Every NSEC(3) RR is linked to the lexicographically next one.

The validation is not affected by :ref:`zone_dnssec-policy` configuration,
except for :ref:`policy_signing-threads` option, which specifies the number
of threads for parallel validation, and :ref:`policy_rrsig-refresh`, which
defines minimal allowed remaining RRSIG validity (otherwise a warning is
logged).

.. NOTE::

   Redundant or garbage NSEC3 records are ignored.

   This mode is not compatible with :ref:`zone_dnssec-signing`.

.. TIP::
   If :ref:`server_dbus-event` is set to ``dnssec-invalid``, a corresponding
   signal is emitted when the validation fails.

*Default:* not set

.. _zone_dnssec-policy:

dnssec-policy
-------------

A :ref:`reference<policy_id>` to DNSSEC signing policy.

.. NOTE::
   A configured policy called "default" won't be used unless explicitly referenced.

*Default:* an imaginary policy with all default values

.. _zone_ds-push:

ds-push
-------

Per zone configuration of :ref:`policy_ds-push`. This option overrides possible
per policy option. Empty value is allowed for template value overriding.

*Default:* not set

.. _zone_zonemd-verify:

zonemd-verify
-------------

On each zone load/update, verify that ZONEMD is present in the zone and valid.

.. NOTE::
   Zone digest calculation may take much time and CPU on large zones.

.. TIP::
   If :ref:`server_dbus-event` is set to ``dnssec-invalid``, a corresponding
   signal is emitted when the verification fails.

*Default:* ``off``

.. _zone_zonemd-generate:

zonemd-generate
---------------

On each zone update, calculate ZONEMD and put it into the zone.

Possible values:

- ``none`` – No action regarding ZONEMD.
- ``zonemd-sha384`` – Generate ZONEMD using SHA384 algorithm.
- ``zonemd-sha512`` – Generate ZONEMD using SHA512 algorithm.
- ``remove`` – Remove any ZONEMD from the zone apex.

*Default:* ``none``

.. _zone_serial-policy:

serial-policy
-------------

Specifies how the zone serial is updated after a dynamic update or
automatic DNSSEC signing. If the serial is changed by the dynamic update,
no change is made.

Possible values:

- ``increment`` – The serial is incremented according to serial number arithmetic.
- ``unixtime`` – The serial is set to the current unix time.
- ``dateserial`` – The 10-digit serial (YYYYMMDDnn) is incremented, the first
  8 digits match the current iso-date.

.. NOTE::
   If the resulting serial for ``unixtime`` or ``dateserial`` is lower than or
   equal to the current serial (this happens e.g. when migrating from other policy or
   frequent updates), the serial is incremented instead.

   To avoid user confusion, use ``dateserial`` only if you expect at most
   100 updates per day per zone and ``unixtime`` only if you expect at most
   one update per second per zone.

   Generated catalog zones use ``unixtime`` only.

*Default:* ``increment`` (``unixtime`` for generated catalog zones)

.. _zone_serial-modulo:

serial-modulo
-------------

The option value is a string consisting of two parts (with no separator between them),
each of which is optional.

The first part specifies that the zone serials must be congruent modulo the specified value.
The format is ``R/M``, where ``R < M <= 256`` are
positive integers. Whenever the zone serial is incremented, it is ensured
that ``serial % M == R``. This can be useful in the case of multiple inconsistent
primaries, where distinct zone serial sequences prevent cross-master-IXFR
by any secondary.

.. NOTE::
   Because the zone serial effectively always increments by ``M`` instead of
   ``1``, it is not recommended to use ``dateserial`` or even ``unixtime``
   :ref:`zone_serial-policy` in the case of rapidly updated zone.

The second part specifies a numeric shift for the generated zone serial.
The shift is formatted as a signed integer, including the sign (``+`` or ``-``).
It is mostly useful with ``unixtime`` :ref:`zone_serial-policy`, where the generated
zone serial is shifted relative to the Unix time.

.. NOTE::
   In order to ensure the congruent policy, this option is only allowed
   with :ref:`DNSSEC signing enabled<zone_dnssec-signing>` and
   :ref:`zone_zonefile-load` to be either ``difference-no-serial`` or ``none``.

*Default:* ``0/1+0``

.. _zone_reverse-generate:

reverse-generate
----------------

This option triggers the automatic generation of reverse PTR records based on
A/AAAA records in the specified zones. The entire generated zone is automatically
stored in the journal.

The auto-generated reverse zone is re-generated whenever any of the specified zones
is updated. This includes the situation when reverse generation had failed due to
some of the specified zones were not yet loaded or had expired.

Current limitations:

- Is slow for large zones (even when changing a little).
- Recomputes all reverse records upon any change in any of the reversed zones.

*Default:* none

.. _zone_refresh-min-interval:

refresh-min-interval
--------------------

Forced minimum zone refresh interval (in seconds) to avoid flooding primary server.

*Minimum:* ``2``

*Default:* ``2``

.. _zone_refresh-max-interval:

refresh-max-interval
--------------------

Forced maximum zone refresh interval (in seconds).

*Default:* not set

.. _zone_retry-min-interval:

retry-min-interval
------------------

Forced minimum zone retry interval (in seconds) to avoid flooding primary server.

*Minimum:* ``1``

*Default:* ``1``

.. _zone_retry-max-interval:

retry-max-interval
------------------

Forced maximum zone retry interval (in seconds).

*Default:* not set

.. _zone_expire-min-interval:

expire-min-interval
-------------------

Forced minimum zone expire interval (in seconds) to avoid flooding primary server.

*Minimum:* ``3``

*Default:* ``3``

.. _zone_expire-max-interval:

expire-max-interval
-------------------

Forced maximum zone expire interval (in seconds).

*Default:* not set

.. _zone_catalog-role:

catalog-role
------------

Trigger zone catalog feature. Possible values:

- ``none`` – Not a catalog zone.
- ``interpret`` – A catalog zone which is loaded from a zone file or XFR,
  and member zones shall be configured based on its contents.
- ``generate`` – A catalog zone whose contents are generated according to
  assigned member zones.
- ``member`` – A member zone that is assigned to one generated catalog zone.

.. NOTE::
   If set to ``generate``, the :ref:`zone_zonefile-load` option has no effect
   since a zone file is never loaded.

*Default:* ``none``

.. _zone_catalog-template:

catalog-template
----------------

For the catalog member zones, the specified configuration template will be applied.

Multiple catalog templates may be defined. The first one is used unless the member zone
has the *group* property defined, matching another catalog template.

.. NOTE::
   This option must be set if and only if :ref:`zone_catalog-role` is *interpret*.

   Nested catalog zones aren't supported. Therefore catalog templates can't
   contain :ref:`zone_catalog-role` set to ``interpret`` or ``generate``.

*Default:* not set

.. _zone_catalog-zone:

catalog-zone
------------

Assign this member zone to specified generated catalog zone.

.. NOTE::
   This option must be set if and only if :ref:`zone_catalog-role` is *member*.

   The referenced catalog zone must exist and have :ref:`zone_catalog-role` set to *generate*.

*Default:* not set

.. _zone_catalog-group:

catalog-group
-------------

Assign this member zone to specified catalog group (configuration template).

.. NOTE::
   This option has effect if and only if :ref:`zone_catalog-role` is *member*.

*Default:* not set

.. _zone_module:

module
------

An ordered list of references to query modules in the form of *module_name* or
*module_name/module_id*. These modules apply only to the current zone queries.

*Default:* not set
