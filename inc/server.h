#pragma once

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast.hpp>

namespace plugin
{
	class server
	{
	public:
		server(int concurrency_hint = 1) :
			_io_context(concurrency_hint),
			_listening(false),
			_port(0),
			_disable_cors(false),
			_disable_static_files(false)
		{}

		virtual ~server() { stop(); }

		void start(const std::string& host, std::uint16_t port) {
			if (running())
				return;

			_host = host;
			_port = port;

			_listening = false;

			_thread = std::thread([this]() {
				boost::asio::co_spawn(_io_context, _async_listener(), boost::asio::detached);

				_io_context.restart();
				_io_context.run();
			});
		}

		void stop() {
			if (!running())
				return;

			_io_context.stop();
			_thread.join();
		}

		bool running() { return _thread.joinable(); }
		std::string host() { return _host; }

		bool wait_for_listening(std::uint32_t ms = 3000) {
			auto ts = std::chrono::steady_clock::now();
			while (!_listening) {
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
				if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - ts).count() > ms) {
					return false;
				}
			}

			return true;
		}

		void set_path(const std::string& path) { _path = path; }
		void set_disable_cors(bool value) { _disable_cors = value; }
		void set_disable_static_files(bool value) { _disable_static_files = value; }

		using handler_t = std::function<boost::asio::awaitable<bool>(
			boost::asio::ip::tcp::socket&,
			boost::beast::http::request<boost::beast::http::string_body>&,
			boost::beast::http::response<boost::beast::http::string_body>&
		)>;

		void map_get(const std::string& url, server::handler_t handler) { _get_handlers[url] = handler; }
		void map_post(const std::string& url, server::handler_t handler) { _post_handlers[url] = handler; }

	private:
		co_async<bool> _async_call_handler(
			boost::asio::ip::tcp::socket& socket,
			boost::beast::http::request<boost::beast::http::string_body>& request,
			boost::beast::http::response<boost::beast::http::string_body>& response,
			const std::unordered_map<std::string, server::handler_t>& handlers
		) {
			auto target = std::string(request.target());
			auto it = handlers.find(target);

			if (it != handlers.end()) {
				if (co_await it->second(socket, request, response)) {
					response.prepare_payload();
					co_await boost::beast::http::async_write(socket, response, boost::asio::use_awaitable);
				}

				co_return true;
			}

			co_return false;
		}

		co_async<void> _async_serve_static_files(
			boost::asio::ip::tcp::socket& socket,
			boost::beast::http::request<boost::beast::http::string_body>& request,
			boost::beast::http::response<boost::beast::http::string_body>& response
		) {
			auto path = std::filesystem::absolute(_path);
			auto target = std::filesystem::path(std::string(request.target())).relative_path();

			path /= target;

			if (std::filesystem::is_directory(path))
				path /= "index.html";

			if (std::filesystem::exists(path) && std::filesystem::is_regular_file(path)) {
				static const std::unordered_map<std::string, std::string> mime_types = {
					{ ".css", "text/css" },
					{ ".htm", "text/html" },
					{ ".html", "text/html" },
					{ ".ico", "image/x-icon" },
					{ ".jpg", "image/jpeg" },
					{ ".js", "application/javascript" },
					{ ".json", "application/json" },
					{ ".png", "image/png" },
					{ ".svg", "image/svg+xml" },
					{ ".txt", "text/plain" },
					{ ".woff", "application/font-woff" },
					{ ".woff2", "font/woff2" },
				};

				auto file = std::ifstream(path, std::ios::in | std::ios::binary);
				auto ext = path.extension().string();

				if (file.is_open()) {
					auto bytes = std::vector<char>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
					file.close();

					response.set(boost::beast::http::field::content_type, mime_types.at(ext));
					response.set(boost::beast::http::field::cache_control, "no-cache");
					response.body().assign(bytes.begin(), bytes.end());
					response.prepare_payload();

					co_await boost::beast::http::async_write(socket, response, boost::asio::use_awaitable);
				}
			}

			co_return;
		}

		co_async<void> _async_handle_request(boost::asio::ip::tcp::socket socket) {
			auto buffer = boost::beast::flat_buffer();
			auto request = boost::beast::http::request<boost::beast::http::string_body>();
			co_await boost::beast::http::async_read(socket, buffer, request, boost::asio::use_awaitable);

			auto response = boost::beast::http::response<boost::beast::http::string_body>{ boost::beast::http::status::ok, request.version() };

			if (_disable_cors) {
				response.set(boost::beast::http::field::access_control_allow_origin, "*");
				response.set(boost::beast::http::field::access_control_allow_headers, "*");
				response.set(boost::beast::http::field::access_control_allow_methods, "GET, POST");
			}

			auto handled = false;

			if (request.method() == boost::beast::http::verb::get) {
				handled = co_await _async_call_handler(socket, request, response, _get_handlers);
			}
			else if (request.method() == boost::beast::http::verb::post) {
				handled = co_await _async_call_handler(socket, request, response, _post_handlers);
			}

			if (!handled && !_disable_static_files)
				co_await _async_serve_static_files(socket, request, response);

			socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both);
		}

		co_async<void> _async_listener() {
			auto resolver = boost::asio::ip::tcp::resolver(_io_context);
			auto query = boost::asio::ip::tcp::resolver::query(_host, std::to_string(_port));
			auto endpoint = boost::asio::ip::tcp::endpoint(*resolver.resolve(query).begin());

			// map IPv6 ::1 to IPv4 127.0.0.1
			if (endpoint.address().is_v6() && endpoint.address() == boost::asio::ip::address::from_string("::1"))
				endpoint = boost::asio::ip::tcp::endpoint(boost::asio::ip::address::from_string("127.0.0.1"), endpoint.port());

			_host = endpoint.address().to_string(); // update host to resolved address for CORS
			auto acceptor = boost::asio::ip::tcp::acceptor(_io_context, endpoint);
			_listening = true;

			while (true)
			{
				try {
					auto socket = co_await acceptor.async_accept(_io_context, boost::asio::use_awaitable);
					boost::asio::co_spawn(_io_context, _async_handle_request(std::move(socket)), boost::asio::detached);
				}
				catch (std::exception&) {
					break;
				}
			}
		}

	private:
		boost::asio::io_context _io_context;
		std::thread _thread;
		std::atomic<bool> _listening;
		std::string _host;
		std::uint16_t _port;
		std::filesystem::path _path;
		bool _disable_cors;
		bool _disable_static_files;
		std::unordered_map<std::string, server::handler_t> _get_handlers;
		std::unordered_map<std::string, server::handler_t> _post_handlers;
	};
}
