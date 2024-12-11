#include <launchdarkly/client_side/client.hpp>
#include <launchdarkly/context_builder.hpp>
#include <sqlite3.h>
#include <cstring>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

using std::filesystem::path;
using launchdarkly::client_side::PersistenceBuilder;


// Set MOBILE_KEY to your LaunchDarkly mobile key.
#define MOBILE_KEY ""

// Set FEATURE_FLAG_KEY to the feature flag key you want to evaluate.
#define FEATURE_FLAG_KEY "my-boolean-flag"

// Set INIT_TIMEOUT_MILLISECONDS to the amount of time you will wait for
// the client to become initialized.
#define INIT_TIMEOUT_MILLISECONDS 3000

char const* get_with_env_fallback(char const* source_val,
                                  char const* env_variable,
                                  char const* error_msg);

using namespace launchdarkly;
using namespace launchdarkly::client_side;

class SQLitePersistence : public IPersistence {
public:
    SQLitePersistence(path dbPath) {
        // Open the database
        
        if (sqlite3_open(dbPath.c_str(), &_db)) {
            std::cerr << "Can't open database: " << sqlite3_errmsg(_db) << std::endl;
            return;
        }

        // Create the table if it doesn't exist
        const char* createTableSQL = R"(
            CREATE TABLE IF NOT EXISTS storage (
                namespace TEXT NOT NULL,
                key TEXT NOT NULL,
                value TEXT,
                PRIMARY KEY (namespace, key)
            );
        )";

        char* errMsg = nullptr;
        if (sqlite3_exec(_db, createTableSQL, nullptr, nullptr, &errMsg) != SQLITE_OK) {
            std::cerr << "SQL error: " << errMsg << std::endl;
            sqlite3_free(errMsg);
        }
    }

    ~SQLitePersistence() {
        if (_db) {
            sqlite3_close(_db);
        }
    }

    void Set(std::string storage_namespace, std::string key, std::string data) noexcept override {
        const char* insertSQL = R"(
            INSERT INTO storage (namespace, key, value) VALUES (?, ?, ?)
            ON CONFLICT(namespace, key) DO UPDATE SET value=excluded.value;
        )";

        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(_db, insertSQL, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, storage_namespace.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, key.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 3, data.c_str(), -1, SQLITE_STATIC);

            if (sqlite3_step(stmt) != SQLITE_DONE) {
                std::cerr << "SQL error: " << sqlite3_errmsg(_db) << std::endl;
            }

            sqlite3_finalize(stmt);
        } else {
            std::cerr << "SQL error: " << sqlite3_errmsg(_db) << std::endl;
        }
    }

    void Remove(std::string storage_namespace, std::string key) noexcept override {
        const char* deleteSQL = R"(
            DELETE FROM storage WHERE namespace = ? AND key = ?;
        )";

        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(_db, deleteSQL, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, storage_namespace.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, key.c_str(), -1, SQLITE_STATIC);

            if (sqlite3_step(stmt) != SQLITE_DONE) {
                std::cerr << "SQL error: " << sqlite3_errmsg(_db) << std::endl;
            }

            sqlite3_finalize(stmt);
        } else {
            std::cerr << "SQL error: " << sqlite3_errmsg(_db) << std::endl;
        }
    }

    std::optional<std::string> Read(std::string storage_namespace, std::string key) noexcept override {
        const char* selectSQL = R"(
            SELECT value FROM storage WHERE namespace = ? AND key = ?;
        )";

        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(_db, selectSQL, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, storage_namespace.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, key.c_str(), -1, SQLITE_STATIC);

            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const unsigned char* value = sqlite3_column_text(stmt, 0);
                sqlite3_finalize(stmt);
                return std::string(reinterpret_cast<const char*>(value));
            }

            sqlite3_finalize(stmt);
        } else {
            std::cerr << "SQL error: " << sqlite3_errmsg(_db) << std::endl;
        }

        return std::nullopt;
    }

private:
    sqlite3* _db = nullptr;
};

class FilePersistence : public IPersistence {
public:
    explicit FilePersistence(std::string basePath) : _basePath(std::move(basePath)) {
        // Ensure the base directory exists
        std::filesystem::create_directories(_basePath);
    }

    void Set(std::string storage_namespace, std::string key, std::string data) noexcept override {
        std::filesystem::path filename = _basePath / (storage_namespace + "-" + key + ".json");
        std::filesystem::path tempFilename = filename.string() + ".tmp";

        try {
            // Write to a temporary file first
            std::ofstream ofs(tempFilename);
            if (!ofs) {
                std::cerr << "Failed to open file for writing: " << tempFilename << std::endl;
                return;
            }

            ofs << data;
            ofs.close();

            // Rename the temporary file to the final file
            std::filesystem::rename(tempFilename, filename);
        } catch (const std::filesystem::filesystem_error& e) {
            std::cerr << "Filesystem error: " << e.what() << std::endl;
        }
    }

    void Remove(std::string storage_namespace, std::string key) noexcept override {
        std::filesystem::path filename = _basePath / (storage_namespace + "-" + key + ".json");
        try {
            std::filesystem::remove(filename);
        } catch (const std::filesystem::filesystem_error& e) {
            std::cerr << "Filesystem error: " << e.what() << std::endl;
        }
    }

    std::optional<std::string> Read(std::string storage_namespace, std::string key) noexcept override {
        std::filesystem::path filename = _basePath / (storage_namespace + "-" + key + ".json");
        try {
            std::ifstream ifs(filename);
            if (!ifs) {
                return std::nullopt;
            }

            std::string data((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            return data;
        } catch (const std::filesystem::filesystem_error& e) {
            std::cerr << "Filesystem error: " << e.what() << std::endl;
            return std::nullopt;
        }
    }

private:
    std::filesystem::path _basePath;
};

path getCacheBaseDirectory() {
    // get the cache environment variable or default to a reasonable default
    // based on the platform
    const char* LD_CACHE_DIR = std::getenv("LD_CACHE_DIR");
    if (LD_CACHE_DIR) {
        return path(LD_CACHE_DIR);
    }
    const char* XDG_CACHE_HOME = std::getenv("XDG_CACHE_HOME");
    if (!XDG_CACHE_HOME) {
        return std::filesystem::temp_directory_path();
    }
    return std::filesystem::temp_directory_path() / "launchdarkly";
};

path getDbPath(path name) {
     
    std::cout << "name is" << std::string(name) << std::endl;
    path fullPath = getCacheBaseDirectory() / name;
   std::cout << "Using cache db: " << fullPath << std::endl;
    fullPath += ".db";
    return fullPath;
}


int main() {
    
    
    char const* mobile_key = get_with_env_fallback(
        MOBILE_KEY, "LD_MOBILE_KEY",
        "Please edit main.c to set MOBILE_KEY to your LaunchDarkly mobile key "
        "first.\n\nAlternatively, set the LD_MOBILE_KEY environment "
        "variable.\n"
        "The value of MOBILE_KEY in main.c takes priority over LD_MOBILE_KEY.");
    const path CACHE_DB = getDbPath(path("example-launchdarkly-cache").filename());
    
    std::cerr << "Using cache db: " << CACHE_DB << std::endl;
    
    
    auto _config = ConfigBuilder(mobile_key);
    _config.Persistence().Custom(std::make_shared<SQLitePersistence>(CACHE_DB));
    auto config = _config.Build();
                
    
    if (!config) {
        std::cout << "error: config is invalid: " << config.error() << '\n';
        return 1;
    }

    auto context =
        ContextBuilder().Kind("user", "example-user-key").Name("Sandy").Build();

    auto client = Client(std::move(*config), std::move(context));

    auto start_result = client.StartAsync();

    if (auto const status = start_result.wait_for(
            std::chrono::milliseconds(INIT_TIMEOUT_MILLISECONDS));
        status == std::future_status::ready) {
        if (start_result.get()) {
            std::cout << "*** SDK successfully initialized!\n\n";
        } else {
            std::cout << "*** SDK failed to initialize\n";
            return 1;
        }
    } else {
        std::cout << "*** SDK initialization didn't complete in "
                  << INIT_TIMEOUT_MILLISECONDS << "ms\n";
        return 1;
    }

    bool const flag_value = client.BoolVariation(FEATURE_FLAG_KEY, false);

    std::cout << "*** Feature flag '" << FEATURE_FLAG_KEY << "' is "
              << (flag_value ? "true" : "false") << " for this user\n\n";

    return 0;
}

char const* get_with_env_fallback(char const* source_val,
                                  char const* env_variable,
                                  char const* error_msg) {
    if (strlen(source_val)) {
        return source_val;
    }

    if (char const* from_env = std::getenv(env_variable);
        from_env && strlen(from_env)) {
        return from_env;
    }

    std::cout << "*** " << error_msg << std::endl;
    std::exit(1);
}
