#include "../components/webui/socket_address.h"
#include <cassert>
int main() {
    using ledcast::webui::socket_has_ipv4;
    sockaddr_storage local{};
    auto* v4=reinterpret_cast<sockaddr_in*>(&local);
    v4->sin_family=AF_INET;
    inet_pton(AF_INET,"192.168.4.1",&v4->sin_addr);
    assert(socket_has_ipv4(local,"192.168.4.1"));
    assert(!socket_has_ipv4(local,"192.168.1.118"));
    assert(!socket_has_ipv4(local,""));
    local={}; auto* v6=reinterpret_cast<sockaddr_in6*>(&local);
    v6->sin6_family=AF_INET6;
    inet_pton(AF_INET6,"::ffff:192.168.4.1",&v6->sin6_addr);
    assert(socket_has_ipv4(local,"192.168.4.1"));
    assert(!socket_has_ipv4(local,"192.168.1.118"));
    inet_pton(AF_INET6,"::ffff:192.168.1.118",&v6->sin6_addr);
    assert(!socket_has_ipv4(local,"192.168.4.1"));
    inet_pton(AF_INET6,"2001:db8::c0a8:401",&v6->sin6_addr);
    assert(!socket_has_ipv4(local,"192.168.4.1"));
}
