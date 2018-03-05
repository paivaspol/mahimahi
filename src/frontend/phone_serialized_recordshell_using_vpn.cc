/* -*-mode:c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
#include <sys/ioctl.h>
#include <sys/socket.h>

#include <linux/if.h>
#include <net/route.h>

#include "address.hh"
#include "config.h"
#include "dns_proxy.hh"
#include "event_loop.hh"
#include "exception.hh"
#include "forwarder.hh"
#include "interfaces.hh"
#include "nat.hh"
#include "netdevice.hh"
#include "noop_store.hh"
#include "serialized_http_proxy.hh"
#include "socketpair.hh"
#include "util.hh"
#include "vpn.hh"

using namespace std;

int main(int argc, char *argv[]) {
  try {
    /* clear environment */
    char **user_environment = environ;
    environ = nullptr;

    check_requirements(argc, argv);

    if (argc < 5) {
      throw runtime_error("Usage: " + string(argv[0]) +
                          " [directory] [prefetch-urls-filename] "
                          "[request-order-filename] [page-url]");
    }

    /* Make sure directory ends with '/' so we can prepend directory to file
     * name for storage */
    string directory(argv[1]);

    if (directory.empty()) {
      throw runtime_error(string(argv[0]) +
                          ": directory name must be non-empty");
    }

    if (directory.back() != '/') {
      directory.append("/");
    }

    const Address nameserver = first_nameserver();

    /* set egress and ingress ip addresses */
    Address egress_addr, ingress_addr;
    tie(egress_addr, ingress_addr) = two_unassigned_addresses();

    /* make pair of devices */
    string egress_name = "veth-" + to_string(getpid()),
           ingress_name = "veth-i" + to_string(getpid());
    VirtualEthernetPair veth_devices(egress_name, ingress_name);

    /* bring up egress */
    assign_address(egress_name, egress_addr, ingress_addr);
    cout << "DNS Outside Nameserver: " << egress_addr.str() << endl;

    /* create DNS proxy */
    DNSProxy dns_outside(egress_addr, nameserver, nameserver);

    /* set up NAT between egress and eth0 */
    NAT nat_rule(ingress_addr);

    /* set up http proxy for tcp */
    string escaped_page_url = escape_page_url(argv[4]);
    SerializedHTTPProxy http_proxy(egress_addr, argv[2], argv[3],
                                   escaped_page_url);

    /* set up dnat */
    DNAT dnat(http_proxy.tcp_listener().local_address(), egress_name);
    DNAT openvpn(Address(ingress_addr.ip(), 1194), "udp", 1194);

    /* prepare event loop */
    EventLoop outer_event_loop;

    /* Fork */
    {
      /* Make pipe for start signal */
      auto pipe = UnixDomainSocket::make_pair();

      ChildProcess container_process(
          "recordshell",
          [&]() {
            /* wait for the go signal */
            pipe.second.read();

            /* bring up localhost */
            interface_ioctl(SIOCSIFFLAGS, "lo",
                            [](ifreq &ifr) { ifr.ifr_flags = IFF_UP; });

            /* bring up veth device */
            assign_address(ingress_name, ingress_addr, egress_addr);

            /* create default route */
            rtentry route;
            zero(route);

            route.rt_gateway = egress_addr.to_sockaddr();
            route.rt_dst = route.rt_genmask = Address().to_sockaddr();
            route.rt_flags = RTF_UP | RTF_GATEWAY;

            SystemCall("ioctl SIOCADDRT",
                       ioctl(UDPSocket().fd_num(), SIOCADDRT, &route));

            /* create DNS proxy if nameserver address is local */
            auto dns_inside = DNSProxy::maybe_proxy(
                nameserver, dns_outside.udp_listener().local_address(),
                dns_outside.tcp_listener().local_address());

            cout << "DNS Inside Nameserver: " << nameserver.str() << endl;
            string path_to_security_files = "/etc/openvpn/";
            VPN vpn(path_to_security_files, ingress_addr);
            vector<string> command = vpn.start_command();

            // For debugging purposes.
            // for (auto i = vpn_command.begin(); i != vpn_command.end(); i++) {
            //   cout << *i << ' ';
            // }
            // cout << endl;
            // vector<string> command = {"bash"};

            /* forward all packets from tun0 (OpenVPN interface) to the ingress
             * interface */
            Forwarder forwarder("tun0", ingress_name);

            /* Fork again after dropping root privileges */
            drop_privileges();

            /* prepare child's event loop */
            EventLoop shell_event_loop;

            shell_event_loop.add_child_process(join(command), [&]() {
              /* restore environment and tweak prompt */
              environ = user_environment;
              prepend_shell_prefix("[record] ");

              return ezexec(command, true);
            });

            if (dns_inside) {
              cout << "DNS Inside not NULL" << endl;
              dns_inside->register_handlers(shell_event_loop);
            }

            return shell_event_loop.loop();
          },
          true); /* new network namespace */

      /* give ingress to container */
      run({IP, "link", "set", "dev", ingress_name, "netns",
           to_string(container_process.pid())});
      veth_devices.set_kernel_will_destroy();

      /* tell ChildProcess it's ok to proceed */
      pipe.first.write("x");

      /* now that we have its pid, move container process to event loop */
      outer_event_loop.add_child_process(move(container_process));
    }

    /* do the actual recording in a different unprivileged child */
    outer_event_loop.add_child_process("recorder", [&]() {
      drop_privileges();

      make_directory(directory);

      /* set up backing store to do nothing */
      NoopStore noop_store;

      EventLoop recordr_event_loop;
      dns_outside.register_handlers(recordr_event_loop);
      http_proxy.register_handlers(recordr_event_loop, noop_store);
      return recordr_event_loop.loop();
    });

    return outer_event_loop.loop();
  } catch (const exception &e) {
    print_exception(e);
    return EXIT_FAILURE;
  }
}
