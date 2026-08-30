// Đăng ký các tuyến API và tuyến phục vụ giao diện web.
#pragma once

#include "http/http_server.h"

namespace ttd {
namespace app {
class App;
}

namespace http {

void registerApiRoutes(HttpServer& server, app::App& app);
void registerStaticRoutes(HttpServer& server, app::App& app);

}  // namespace http
}  // namespace ttd
