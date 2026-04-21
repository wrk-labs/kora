#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "httplib.h"

extern "C" {
#include "internal.h"
}

namespace {

httplib::Server *g_srv = nullptr;
std::mutex g_srv_mu;

/* best-effort scan for a top-level "model" string field in a JSON body.
   adequate for stage 2 — we log but don't route on the value yet. */
std::string extract_model(const std::string &body)
{
	auto key = body.find("\"model\"");
	if (key == std::string::npos) return {};
	auto colon = body.find(':', key + 7);
	if (colon == std::string::npos) return {};
	auto q1 = body.find('"', colon + 1);
	if (q1 == std::string::npos) return {};
	auto q2 = body.find('"', q1 + 1);
	if (q2 == std::string::npos) return {};
	return body.substr(q1 + 1, q2 - q1 - 1);
}

httplib::Headers forwardable_headers(const httplib::Headers &in)
{
	httplib::Headers out;
	for (const auto &kv : in) {
		if (kv.first == "Host" ||
		    kv.first == "Content-Length" ||
		    kv.first == "Accept-Encoding" ||
		    kv.first == "Connection")
			continue;
		out.insert(kv);
	}
	return out;
}

bool looks_streaming(const httplib::Request &req)
{
	if (req.method != "POST") return false;
	return req.path.find("/chat/completions") != std::string::npos ||
	       req.path.find("/completions")      != std::string::npos ||
	       req.path.find("/embeddings")       != std::string::npos;
}

void proxy_request(const httplib::Request &req, httplib::Response &res)
{
	std::string requested = extract_model(req.body);
	int port = kora_child_ensure_ready(
		requested.empty() ? nullptr : requested.c_str());
	if (port < 0) {
		res.status = 503;
		res.set_content(
			R"({"error":{"message":"model unavailable","type":"kora_unavailable"}})",
			"application/json");
		return;
	}

	if (looks_streaming(req)) {
		/* capture by value — the provider runs after proxy_request returns */
		std::string path = req.path;
		std::string body = req.body;
		std::string ctype = req.get_header_value("Content-Type");
		httplib::Headers hdrs = forwardable_headers(req.headers);

		res.set_chunked_content_provider(
			"text/event-stream",
			[port, path = std::move(path), body = std::move(body),
			 ctype = std::move(ctype), hdrs = std::move(hdrs)]
			(size_t /*offset*/, httplib::DataSink &sink) {
				kora_server_begin_request();
				httplib::Client cli("127.0.0.1", port);
				cli.set_read_timeout(std::chrono::hours(1));
				cli.set_connection_timeout(std::chrono::seconds(30));
				cli.Post(path, hdrs, body,
				         ctype.empty() ? "application/json" : ctype.c_str(),
				         [&sink](const char *data, size_t len) {
					         return sink.write(data, len);
				         });
				sink.done();
				kora_server_end_request();
				return true;
			});
		return;
	}

	/* non-streaming: sync forward */
	kora_server_begin_request();

	httplib::Client cli("127.0.0.1", port);
	cli.set_read_timeout(std::chrono::seconds(30));
	cli.set_connection_timeout(std::chrono::seconds(30));
	httplib::Headers hdrs = forwardable_headers(req.headers);

	httplib::Result r;
	if (req.method == "POST") {
		std::string ctype = req.get_header_value("Content-Type");
		r = cli.Post(req.path, hdrs, req.body,
		             ctype.empty() ? "application/json" : ctype.c_str());
	} else if (req.method == "DELETE") {
		r = cli.Delete(req.path, hdrs);
	} else {
		r = cli.Get(req.path, hdrs);
	}

	if (!r) {
		res.status = 502;
		res.set_content(
			R"({"error":{"message":"upstream unavailable"}})",
			"application/json");
	} else {
		res.status = r->status;
		std::string ct = r->get_header_value("Content-Type");
		res.set_content(r->body, ct.empty() ? "application/json" : ct);
	}

	kora_server_end_request();
}

} // namespace

extern "C" int kora_proxy_listen(int port)
{
	{
		std::lock_guard<std::mutex> lk(g_srv_mu);
		g_srv = new httplib::Server();
	}
	g_srv->set_payload_max_length(32 * 1024 * 1024);

	auto handler = [](const httplib::Request &req, httplib::Response &res) {
		proxy_request(req, res);
	};
	g_srv->Get(R"(.*)",    handler);
	g_srv->Post(R"(.*)",   handler);
	g_srv->Put(R"(.*)",    handler);
	g_srv->Delete(R"(.*)", handler);
	g_srv->Patch(R"(.*)",  handler);

	bool ok = g_srv->listen("127.0.0.1", port);

	{
		std::lock_guard<std::mutex> lk(g_srv_mu);
		delete g_srv;
		g_srv = nullptr;
	}
	return ok ? 0 : 1;
}

extern "C" void kora_proxy_stop(void)
{
	std::lock_guard<std::mutex> lk(g_srv_mu);
	if (g_srv) g_srv->stop();
}
