#include "elevsafe/retina_client.h"
#include "elevsafe/radar_reconnect.h"
#include <boost/asio.hpp>
#include <chrono>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace
{
    void expect(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

    // Isolated fixture process: consumes bytes produced by the existing PowerShell Make-Packet.
    // Process exit makes server shutdown/restart unambiguous, without sharing a Windows port.
    int serve(const char* port, const char* payload, const char* gate, const char* ending)
    {
        namespace net = boost::asio;
        using Clock = std::chrono::steady_clock;
        std::ifstream input(payload, std::ios::binary);
        expect(input.good(), "fixture payload missing");
        std::vector<char> bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        net::io_context io;
        net::ip::tcp::acceptor acceptor(io);
        acceptor.open(net::ip::tcp::v4());
        acceptor.set_option(net::socket_base::reuse_address(true)); // Only after prior server process exits.
        acceptor.bind({net::ip::address_v4::loopback(), static_cast<unsigned short>(std::stoi(port))});
        acceptor.listen(8);
        acceptor.non_blocking(true);
        std::cout << "READY port=" << acceptor.local_endpoint().port() << std::endl;
        const auto deadline = Clock::now() + std::chrono::seconds(8);
        net::ip::tcp::socket peer(io);
        boost::system::error_code error;
        for (;;)
        {
            acceptor.accept(peer, error);
            if (!error) break;
            if (error != net::error::would_block && error != net::error::try_again)
                throw boost::system::system_error(error, "fixture.accept");
            expect(Clock::now() < deadline, "fixture accept timeout");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        while (!std::filesystem::exists(gate))
        {
            expect(Clock::now() < deadline, "fixture gate timeout");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        net::write(peer, net::buffer(bytes));
        if (std::string_view(ending) == "rst")
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            peer.set_option(net::socket_base::linger(true, 0));
            peer.close();
        }
        else
        {
            peer.shutdown(net::ip::tcp::socket::shutdown_send);
            peer.non_blocking(true);
            char byte;
            for (;;)
            {
                peer.read_some(net::buffer(&byte, 1), error);
                if (error == net::error::eof) break;
                if (error && error != net::error::would_block && error != net::error::try_again)
                    throw boost::system::system_error(error, "fixture.fin");
                expect(Clock::now() < deadline, "fixture FIN timeout");
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        std::cout << "DONE " << ending << std::endl;
        return 0;
    }
}

int main(int argc, char** argv)
{
    using Client = elevsafe::RetinaClient;
    using Budget = elevsafe::RadarReconnectBudget;
    using Clock = Budget::Clock;
    try
    {
        if (argc == 6 && std::string_view(argv[1]) == "--server")
            return serve(argv[2], argv[3], argv[4], argv[5]);
        expect(argc == 1, "expected no arguments or --server PORT PAYLOAD GATE fin|rst");
        const Clock::time_point origin{};
        Budget count;
        expect(count.schedule(origin, false) == origin + std::chrono::seconds(1), "retry delay");
        expect(count.schedule(origin + std::chrono::seconds(2), true).has_value(), "second outage");
        expect(count.schedule(origin + std::chrono::seconds(4), true).has_value(), "third retry");
        expect(!count.schedule(origin + std::chrono::seconds(6), true), "healthy connection reset global retry cap");
        Budget budget;
        budget.schedule(origin, false);
        expect(!budget.schedule(origin + std::chrono::seconds(14), false), "recovery time budget not enforced");

        Client::RunDiagnostics status;
        status.stage = "connect";
#ifdef _WIN32
        status.errorCode = WSAEFAULT;
        expect(!Client::retryable(status, true), "10014 must not be retried");
        status.errorCode = WSAEADDRINUSE;
        expect(!Client::retryable(status, true), "10048 must not be retried");
        status.errorCode = WSAECONNREFUSED;
#else
        status.errorCode = ECONNREFUSED;
#endif
        expect(Client::retryable(status, false), "refusal must be recoverable");
        status.stage = "resolve";
        expect(!Client::retryable(status, true), "resolution is not retryable");
        status.stage = "eof";
        expect(!Client::retryable(status, false) && Client::retryable(status, true), "EOF opt-in");
        status.cancelled = true;
        expect(!Client::retryable(status, true), "cancellation must not reconnect");

        std::ostringstream log;
        Client client("127.0.0.1", 1);
        Client::SessionControl control;
        control.cancelRequested = [] { return true; };
        expect(client.runSession(log, control, status) == 0 && status.cancelled && !status.connected,
            "pre-connect cancellation");
        control.cancelRequested = {};
        control.connectNotAfter = Clock::now() - std::chrono::seconds(1);
        expect(client.runSession(log, control, status) == 1 && status.timedOut && status.stage == "start",
            "expired recovery deadline");

        // Kernel loopback connect completes without a server worker; no application bytes follow.
        // This deliberately exercises cancellable receive with no idle timeout.
        namespace net = boost::asio;
        net::io_context io;
        net::ip::tcp::acceptor listener(io);
        listener.open(net::ip::tcp::v4());
        listener.bind({net::ip::address_v4::loopback(), 0});
        listener.listen(8);
        Client idle("127.0.0.1", listener.local_endpoint().port());
        control.connectNotAfter.reset();
        const auto start = Clock::now();
        control.cancelRequested = [start] { return Clock::now() - start >= std::chrono::milliseconds(200); };
        expect(idle.runSession(log, control, status) == 0 && status.cancelled && status.connected
            && status.stage == "stopped", "idle receive cancellation");
        expect(Clock::now() - start < std::chrono::seconds(2), "receive cancellation took too long");
        std::cout << "retina_reconnect_test passed: policy, 10014 rejection, expired deadline, owner-thread receive cancellation\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "retina_reconnect_test failed: " << error.what() << '\n';
        return 1;
    }
}
