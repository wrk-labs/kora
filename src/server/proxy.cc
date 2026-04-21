#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>

#include "httplib.h"

extern "C" {
#include "internal.h"
}

namespace {

httplib::Server *g_srv = nullptr;
std::mutex g_srv_mu;

/* --- helpers --- */

/* naive JSON string-field extractor: returns the string value of top-level
   key, or empty. adequate for us — we only parse known shapes. */
std::string json_str_field(const std::string &body, const char *key)
{
	std::string needle = std::string("\"") + key + "\"";
	auto k = body.find(needle);
	if (k == std::string::npos) return {};
	auto colon = body.find(':', k + needle.size());
	if (colon == std::string::npos) return {};
	auto q1 = body.find('"', colon + 1);
	if (q1 == std::string::npos) return {};
	auto q2 = body.find('"', q1 + 1);
	if (q2 == std::string::npos) return {};
	return body.substr(q1 + 1, q2 - q1 - 1);
}

std::string json_escape(const std::string &in)
{
	std::string out;
	out.reserve(in.size() + 8);
	for (char c : in) {
		switch (c) {
		case '"':  out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n";  break;
		case '\r': out += "\\r";  break;
		case '\t': out += "\\t";  break;
		default:
			if ((unsigned char)c < 0x20)
				out += "?";  // cheap; real control chars shouldn't appear
			else
				out += c;
		}
	}
	return out;
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

/* --- admin endpoints --- */

void admin_status(httplib::Response &res)
{
	struct kora_pool_snap snaps[8];
	int n = kora_pool_snapshot(snaps, 8);

	std::ostringstream os;
	os << "{"
	   << "\"pool_cap\":"           << kora_pool_cap()        << ","
	   << "\"ctx_size\":"           << kora_pool_ctx_size()   << ","
	   << "\"parallel\":"           << kora_pool_parallel()   << ","
	   << "\"default_model\":\""    << json_escape(kora_pool_default_model()) << "\","
	   << "\"loaded\":[";
	for (int i = 0; i < n; i++) {
		if (i) os << ",";
		os << "{"
		   << "\"model\":\""  << json_escape(snaps[i].model) << "\","
		   << "\"pid\":"       << snaps[i].pid        << ","
		   << "\"port\":"      << snaps[i].port       << ","
		   << "\"in_flight\":" << snaps[i].in_flight  << ","
		   << "\"idle_secs\":" << snaps[i].idle_secs  << ","
		   << "\"loading\":"   << (snaps[i].loading ? "true" : "false")
		   << "}";
	}
	os << "]}";
	res.status = 200;
	res.set_content(os.str(), "application/json");
}

extern "C" struct registry_entry {
	const char *alias;
	const char *url;
	const char *size;
	const char *quant;
};
extern "C" struct registry_entry registry[];

void admin_models(httplib::Response &res)
{
	/* cross-reference registry with which are currently loaded */
	struct kora_pool_snap snaps[8];
	int n_loaded = kora_pool_snapshot(snaps, 8);

	std::ostringstream os;
	os << "{\"models\":[";
	bool first = true;
	for (int i = 0; registry[i].alias; i++) {
		if (!first) os << ",";
		first = false;
		bool loaded = false;
		for (int j = 0; j < n_loaded; j++) {
			if (std::strcmp(snaps[j].model, registry[i].alias) == 0) {
				loaded = true; break;
			}
		}
		os << "{"
		   << "\"alias\":\"" << json_escape(registry[i].alias) << "\","
		   << "\"size\":\""  << json_escape(registry[i].size)  << "\","
		   << "\"quant\":\"" << json_escape(registry[i].quant) << "\","
		   << "\"loaded\":"  << (loaded ? "true" : "false")
		   << "}";
	}
	os << "]}";
	res.status = 200;
	res.set_content(os.str(), "application/json");
}

void admin_unload(const httplib::Request &req, httplib::Response &res)
{
	std::string model = json_str_field(req.body, "model");
	if (model.empty()) {
		res.status = 400;
		res.set_content(R"({"error":{"message":"model field required"}})",
		                "application/json");
		return;
	}
	int rc = kora_pool_unload(model.c_str());
	if (rc == -1) {
		res.status = 404;
		res.set_content(R"({"error":{"message":"not loaded"}})", "application/json");
	} else if (rc == -2) {
		res.status = 409;
		res.set_content(R"({"error":{"message":"busy","hint":"wait for in-flight requests"}})",
		                "application/json");
	} else {
		res.status = 200;
		std::ostringstream os;
		os << "{\"unloaded\":\"" << json_escape(model) << "\"}";
		res.set_content(os.str(), "application/json");
	}
}

bool handle_admin(const httplib::Request &req, httplib::Response &res)
{
	if (req.method == "GET" && req.path == "/kora/status") {
		admin_status(res); return true;
	}
	if (req.method == "GET" && req.path == "/kora/models") {
		admin_models(res); return true;
	}
	if (req.method == "POST" && req.path == "/kora/unload") {
		admin_unload(req, res); return true;
	}
	res.status = 404;
	res.set_content(R"({"error":{"message":"unknown kora admin endpoint"}})",
	                "application/json");
	return true;
}

/* --- forwarding --- */

void proxy_request(const httplib::Request &req, httplib::Response &res)
{
	if (req.path.rfind("/kora/", 0) == 0) {
		handle_admin(req, res);
		return;
	}

	std::string requested = json_str_field(req.body, "model");
	int slot = -1;
	int port = kora_pool_ensure_ready(
		requested.empty() ? nullptr : requested.c_str(), &slot);
	if (port < 0) {
		res.status = 503;
		res.set_content(
			R"({"error":{"message":"model unavailable","type":"kora_unavailable"}})",
			"application/json");
		return;
	}

	/* from here on: MUST call kora_pool_release(slot) exactly once. */

	if (looks_streaming(req)) {
		std::string path = req.path;
		std::string body = req.body;
		std::string ctype = req.get_header_value("Content-Type");
		httplib::Headers hdrs = forwardable_headers(req.headers);

		res.set_chunked_content_provider(
			"text/event-stream",
			[port, slot, path = std::move(path), body = std::move(body),
			 ctype = std::move(ctype), hdrs = std::move(hdrs)]
			(size_t /*offset*/, httplib::DataSink &sink) {
				httplib::Client cli("127.0.0.1", port);
				cli.set_read_timeout(std::chrono::hours(1));
				cli.set_connection_timeout(std::chrono::seconds(30));
				cli.Post(path, hdrs, body,
				         ctype.empty() ? "application/json" : ctype.c_str(),
				         [&sink](const char *data, size_t len) {
					         return sink.write(data, len);
				         });
				sink.done();
				kora_pool_release(slot);
				return true;
			});
		return;
	}

	/* sync forward */
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

	kora_pool_release(slot);
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
