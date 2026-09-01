// Tuấn's Telegram Disk — điểm khởi động chương trình.
// Thiết kế bởi Tuandethuong.

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

#include "app/app.h"
#include "common/config.h"
#include "common/fsutil.h"
#include "common/logging.h"
#include "common/net.h"
#include "common/strutil.h"
#include "common/timeutil.h"
#include "http/assets.h"
#include "tg/tl_schema.h"
#include "version.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

std::atomic<bool>* g_shutdownFlag = nullptr;

void handleSignal(int sig) {
    (void)sig;
    if (g_shutdownFlag) g_shutdownFlag->store(true);
}

void printBanner() {
    std::printf(
        "\n"
        "  ╔══════════════════════════════════════════════════════════════╗\n"
        "  ║              T U Ấ N ' S   T E L E G R A M   D I S K          ║\n"
        "  ║        Ổ đĩa lưu trữ không giới hạn trên nền Telegram        ║\n"
        "  ╚══════════════════════════════════════════════════════════════╝\n"
        "   Phiên bản %s (build %d) — %s\n"
        "   Múi giờ: %s   |   %s\n"
        "   %s\n\n",
        ttd::version::kVersion, ttd::version::kBuildNumber, ttd::version::kGitCommit,
        ttd::kSystemTimezoneName, ttd::formatDateTime(ttd::nowUnix()).c_str(),
        ttd::version::kAppFooter);
}

void printHelp(const char* program) {
    std::printf(
        "Cách dùng: %s [tuỳ chọn]\n\n"
        "Tuỳ chọn:\n"
        "  -c, --config <tệp>     Đường dẫn tệp cấu hình (mặc định: config.json cạnh\n"
        "                         tệp thực thi)\n"
        "  -p, --port <cổng>      Ghi đè cổng của máy chủ web\n"
        "  -b, --bind <địa chỉ>   Ghi đè địa chỉ lắng nghe\n"
        "      --data <thư mục>   Thư mục dữ liệu gốc\n"
        "      --check-schema     Kiểm tra tệp schema TL rồi thoát\n"
        "      --print-config     In cấu hình hiện tại rồi thoát\n"
        "  -v, --version          In phiên bản rồi thoát\n"
        "  -h, --help             Hiện trợ giúp này\n\n"
        "Ví dụ:\n"
        "  %s --config /etc/tuan-telegram-disk.json --port 9000\n\n"
        "Sau khi chạy, mở trình duyệt tới http://<địa-chỉ-máy>:<cổng>\n"
        "Lần chạy đầu tiên, mật khẩu quản trị sẽ được in ra màn hình.\n\n"
        "%s\n",
        program, program, ttd::version::kAppFooter);
}

int runCheckSchema() {
    using namespace ttd;
    Logger::instance().configure(LogLevel::Info, "", 0, 1, true, 100);

    tg::TlSchema schema;
    std::vector<std::string> warnings;
    std::string content;

    Config& cfg = Config::instance();
    cfg.setDataRoot(executableDirectory());

    if (!assets::find("schema/mtproto.tl", content)) {
        std::printf("LỖI: thiếu schema/mtproto.tl\n");
        return 1;
    }
    schema.load(content, &warnings);

    std::string apiPath = cfg.resolvePath("schema/api.tl");
    if (readWholeFile(apiPath, content) && !content.empty()) {
        std::printf("Dùng schema bên ngoài: %s\n", apiPath.c_str());
    } else if (!assets::find("schema/api.tl", content)) {
        std::printf("LỖI: thiếu schema/api.tl\n");
        return 1;
    } else {
        std::printf("Dùng schema đi kèm trong tệp thực thi.\n");
    }
    schema.load(content, &warnings);

    std::printf("Đã nạp %zu hàm dựng, layer %d\n", schema.size(), schema.layer());
    if (warnings.empty()) {
        std::printf("Không có cảnh báo — mọi định danh khớp với CRC32 của khai báo.\n");
    } else {
        std::printf("\nCảnh báo (%zu):\n", warnings.size());
        for (const auto& w : warnings) std::printf("  - %s\n", w.c_str());
    }

    static const char* kRequired[] = {
        "req_pq_multi", "req_DH_params", "set_client_DH_params", "invokeWithLayer",
        "initConnection", "help.getConfig", "upload.saveBigFilePart", "upload.saveFilePart",
        "upload.getFile", "messages.sendMedia", "inputMediaUploadedDocument", "inputFileBig",
        "inputPeerChannel", "inputChannel", "inputDocumentFileLocation", "documentAttributeFilename",
        "channels.getMessages", "channels.deleteMessages", "channels.getChannels",
        "contacts.resolveUsername", "messages.getDialogs", "auth.sendCode", "auth.signIn",
        "auth.checkPassword", "auth.exportAuthorization", "auth.importAuthorization",
        "account.getPassword", "codeSettings", "users.getUsers", "document", "message",
        "messageMediaDocument", "updates", "updateNewChannelMessage", "updateMessageID",
        "config", "dcOption", "channel", "user", "upload.file", nullptr};

    std::printf("\nCác hàm dựng ứng dụng cần dùng:\n");
    int missing = 0;
    for (int i = 0; kRequired[i]; ++i) {
        const tg::TlConstructor* c = schema.byName(kRequired[i]);
        if (c) {
            std::printf("  ✓ %-32s #%08x\n", kRequired[i], c->id);
        } else {
            std::printf("  ✗ %-32s THIẾU\n", kRequired[i]);
            ++missing;
        }
    }
    if (missing) {
        std::printf(
            "\nThiếu %d hàm dựng. Hãy tải tệp api.tl đầy đủ của layer đang dùng và đặt\n"
            "vào thư mục schema/ cạnh tệp thực thi.\n",
            missing);
        return 1;
    }
    std::printf("\nSchema đầy đủ. Sẵn sàng kết nối Telegram.\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    // Bảo đảm hiển thị tiếng Việt đúng trên Command Prompt.
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

#if defined(__GLIBC__)
    // Ứng dụng cấp phát/giải phóng liên tục các khối 1 MB khi đọc dữ liệu về.
    // glibc có "ngưỡng mmap động": lần đầu giải phóng một khối mmap, nó nâng
    // ngưỡng lên bằng kích thước khối đó, nên các khối 1 MB sau này lấy từ heap
    // và KHÔNG bao giờ trả lại cho hệ điều hành. Đo thực tế: tải một tệp 1,85 GB
    // sáu lần làm RSS leo từ 268 MB lên 1024 MB dù bộ đệm vẫn giữ đúng mức 256 MB.
    // Ghim ngưỡng lại thì các khối 1 MB luôn đi qua mmap và được trả lại ngay —
    // RSS đứng yên ở 265 MB qua cả sáu lần.
    mallopt(M_MMAP_THRESHOLD, 256 * 1024);
    mallopt(M_TRIM_THRESHOLD, 4 * 1024 * 1024);
#endif

    using namespace ttd;

    std::string configPath;
    std::string dataRootOverride;
    int portOverride = 0;
    std::string bindOverride;
    bool checkSchema = false;
    bool printConfig = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&](std::string& out) {
            if (i + 1 < argc) out = argv[++i];
        };
        if (arg == "-h" || arg == "--help") {
            printHelp(argv[0]);
            return 0;
        }
        if (arg == "-v" || arg == "--version") {
            std::printf("%s %s (build %d, %s, %s)\n%s\n", version::kAppName, version::kVersion,
                        version::kBuildNumber, version::kGitCommit, version::kBuildTimeUtc,
                        version::kAppFooter);
            return 0;
        }
        if (arg == "-c" || arg == "--config") {
            next(configPath);
            continue;
        }
        if (arg == "--data") {
            next(dataRootOverride);
            continue;
        }
        if (arg == "-b" || arg == "--bind") {
            next(bindOverride);
            continue;
        }
        if (arg == "-p" || arg == "--port") {
            std::string value;
            next(value);
            int64_t v = 0;
            if (parseInt64(value, v) && v > 0 && v < 65536) portOverride = static_cast<int>(v);
            continue;
        }
        if (arg == "--check-schema") {
            checkSchema = true;
            continue;
        }
        if (arg == "--print-config") {
            printConfig = true;
            continue;
        }
        std::fprintf(stderr, "Tuỳ chọn không hiểu: %s (dùng --help để xem trợ giúp)\n",
                     arg.c_str());
        return 2;
    }

    if (checkSchema) return runCheckSchema();

    printBanner();
    net::initNetworking();

    Config& cfg = Config::instance();
    if (!dataRootOverride.empty()) cfg.setDataRoot(absolutePath(dataRootOverride));

    app::App application;
    std::string error;
    if (!application.start(configPath, error)) {
        std::fprintf(stderr, "\n  ✗ Không khởi động được: %s\n\n", error.c_str());
        return 1;
    }

    // Ghi đè sau khi nạp cấu hình (chỉ ảnh hưởng lần chạy này).
    if (portOverride || !bindOverride.empty()) {
        LOG_WARN("main",
                 "Cổng/địa chỉ chỉ định trên dòng lệnh cần đặt trong tệp cấu hình để có "
                 "hiệu lực lâu dài.");
    }

    if (printConfig) {
        std::printf("%s\n", cfg.toJson().dump(2).c_str());
        application.stop();
        return 0;
    }

    std::atomic<bool> shutdown{false};
    g_shutdownFlag = &shutdown;
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    LOG_INFO("main", "Nhấn Ctrl+C để dừng.");
    while (!shutdown.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    LOG_INFO("main", "Nhận tín hiệu dừng…");
    application.stop();
    return 0;
}
