// Giao thức WebDAV: cho phép gắn ổ đĩa vào Windows Explorer, macOS Finder,
// Linux (GVFS/davfs2), hoặc phát trực tuyến bằng VLC / Kodi / PotPlayer.
#pragma once

#include "http/http_server.h"

namespace ttd {
namespace app {
class App;
}

namespace http {

void registerWebdavRoutes(HttpServer& server, app::App& app);

}  // namespace http
}  // namespace ttd
