// CEnglish.cpp

#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace fs { // File System
    class path {
        public:
            path() = default;
            path(const char* s) : p_(s ? s : "") {}
            path(const std::string& s) : p_(s) {}

            const std::string& string() const noexcept { return p_; }

        private:
            std::string p_;
    };

    inline bool exists(const path& p) {
        std::ifstream in(p.string(), std::ios::binary);
        return static_cast<bool>(in);
    }

    inline path absolute(const path& p) {
        return p;
    }
}

namespace CEnglish {

    using Token = std::string;
    using Source = std::string;

    struct Value {
        std::variant<std::monostate, long long, double, bool, std::string> data{};

        Value() = default;
        Value(long long v) : data(v) {}
        Value(double v) : data(v) {}
        Value(bool v) : data(v) {}
        Value(std::string v) : data(std::move(v)) {}
        Value(const char* v) : data(std::string(v)) {}

        bool is_null() const { return std::holds_alternative<std::monostate>(data); }
        bool is_int() const { return std::holds_alternative<long long>(data); }
        bool is_double() const { return std::holds_alternative<double>(data); }
        bool is_bool() const { return std::holds_alternative<bool>(data); }
        bool is_string() const { return std::holds_alternative<std::string>(data); }

        long long as_int(long long fallback = 0) const {
            if (auto p = std::get_if<long long>(&data)) return *p;
            if (auto p = std::get_if<double>(&data)) return static_cast<long long>(*p);
            if (auto p = std::get_if<bool>(&data)) return *p ? 1LL : 0LL;
            if (auto p = std::get_if<std::string>(&data)) {
                try { return std::stoll(*p); } catch (...) { return fallback; }
            }
            return fallback;
        }

        double as_double(double fallback = 0.0) const {
            if (auto p = std::get_if<double>(&data)) return *p;
            if (auto p = std::get_if<long long>(&data)) return static_cast<double>(*p);
            if (auto p = std::get_if<bool>(&data)) return *p ? 1.0 : 0.0;
            if (auto p = std::get_if<std::string>(&data)) {
                try { return std::stod(*p); } catch (...) { return fallback; }
            }
            return fallback;
        }

        bool as_bool(bool fallback = false) const {
            if (auto p = std::get_if<bool>(&data)) return *p;
            if (auto p = std::get_if<long long>(&data)) return *p != 0;
            if (auto p = std::get_if<double>(&data)) return *p != 0.0;
            if (auto p = std::get_if<std::string>(&data)) {
                if (*p == "true" || *p == "1") return true;
                if (*p == "false" || *p == "0" || p->empty()) return false;
                return fallback;
            }
            return fallback;
        }

        std::string as_string() const {
            if (auto p = std::get_if<std::string>(&data)) return *p;
            if (auto p = std::get_if<long long>(&data)) return std::to_string(*p);
            if (auto p = std::get_if<double>(&data)) {
                std::ostringstream oss;
                oss << std::setprecision(17) << *p;
                return oss.str();
            }
            if (auto p = std::get_if<bool>(&data)) return *p ? "true" : "false";
            return {};
        }
    };

    struct TokenSpec;
    class Runtime;

    enum class Kind {
        NoOp,
        Define,
        Update,
        Delete,
        View,
        List,
        Output,
        Input,
        Query,
        StackPush,
        StackPop,
        StackDup,
        StackSwap,
        Assign,
        Retrieve,
        Arithmetic,
        Compare,
        Logic,
        Conditional,
        Loop,
        Call,
        Return,
        IncludeFile,
        SaveFile,
        LoadFile,
        Autocomplete,
        TimeQuery,
        CustomMacro
    };

    using Handler = std::function<void(Runtime&, const std::vector<std::string>&)>;

    struct TokenSpec {
        Token name;
        Kind kind{Kind::NoOp};
        std::size_t arity{0};
        std::vector<std::string> param_names;
        std::string description;
        Handler handler;
        bool builtin{false};
    };

    struct CustomToken {
        Token name;
        std::vector<std::string> param_names;
        std::vector<std::string> definition_tokens;
        std::string description;
    };

    struct Diagnostic {
        std::size_t line = 0;
        std::size_t column = 0;
        std::string message;
        std::vector<std::string> suggestions;
    };

    static std::string trim(const std::string& s) {
        std::size_t a = 0, b = s.size();
        while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
        while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
        return s.substr(a, b - a);
    }

    static std::vector<std::string> split_ws(const std::string& line) {
        std::istringstream iss(line);
        std::vector<std::string> out;
        for (std::string tok; iss >> tok;) out.push_back(std::move(tok));
        return out;
    }

    static bool starts_with(const std::string& s, const std::string& prefix) {
        return s.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), s.begin());
    }

    static std::string join_tokens(const std::vector<std::string>& toks, std::size_t from = 0) {
        std::ostringstream oss;
        for (std::size_t i = from; i < toks.size(); ++i) {
            if (i != from) oss << ' ';
            oss << toks[i];
        }
        return oss.str();
    }

    static long long edit_distance(const std::string& a, const std::string& b) {
        const std::size_t n = a.size(), m = b.size();
        std::vector<long long> prev(m + 1), cur(m + 1);
        for (std::size_t j = 0; j <= m; ++j) prev[j] = static_cast<long long>(j);
        for (std::size_t i = 1; i <= n; ++i) {
            cur[0] = static_cast<long long>(i);
            for (std::size_t j = 1; j <= m; ++j) {
                long long cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
                cur[j] = std::min({ prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost });
            }
            prev.swap(cur);
        }
        return prev[m];
    }

    class Runtime {
    public:
        Runtime() {
            register_builtins();
            load_user_tokens(default_db_path());
        }

        void repl() {
            std::string line;
            while (true) {
                std::cout << "> ";
                if (!std::getline(std::cin, line)) break;
                line = trim(line);
                if (line.empty()) continue;
                if (line == ":quit" || line == ":exit") break;
                if (!line.empty() && line[0] == ':') {
                    handle_meta_command(line);
                    continue;
                }
                execute_line(line, "<stdin>");
            }
        }

        int run_file(const fs::path& path) {
            if (!fs::exists(path)) {
                std::cerr << "Error: file not found: " << path.string() << "\n";
                return 1;
            }
            std::unordered_set<std::string> stack;
            try {
                if (has_suffix(path.string(), ".ce")) {
                    load_source_code(path);
                    return 0;
                }
                execute_file(path, stack);
            } catch (const std::exception& e) {
                std::cerr << "Runtime error: " << e.what() << "\n";
                return 1;
            }
            return 0;
        }

    private:
        std::unordered_map<Token, TokenSpec> builtins_;
        std::unordered_map<Token, CustomToken> custom_;
        std::unordered_map<Token, Value> vars_;
        std::vector<Value> stack_;
        std::vector<Diagnostic> diagnostics_;
        std::vector<fs::path> include_stack_;
        std::unordered_set<std::string> include_guard_;

        static fs::path default_db_path() {
            return fs::path("CEnglish.db");
        }

        bool has_builtin(const std::string& name) const {
            return builtins_.find(name) != builtins_.end();
        }

        bool has_custom(const std::string& name) const {
            return custom_.find(name) != custom_.end();
        }

        bool is_variable_name(const std::string& name) const {
            return vars_.find(name) != vars_.end();
        }

        bool is_keytoken_name(const std::string& name) const {
            return has_builtin(name) || has_custom(name);
        }

        bool is_reserved_name(const std::string& name) const {
            return is_keytoken_name(name) || is_variable_name(name);
        }

        void ensure_param_names_valid(const std::vector<std::string>& params, const std::string& token_name) const {
            std::unordered_set<std::string> seen;
            for (const auto& p : params) {
                if (p.empty()) {
                    throw std::runtime_error("empty parameter name in token: " + token_name);
                }
                if (is_keytoken_name(p)) {
                    throw std::runtime_error("parameter name '" + p + "' conflicts with an existing keytoken");
                }
                if (!seen.insert(p).second) {
                    throw std::runtime_error("duplicate parameter name '" + p + "' in token: " + token_name);
                }
            }
        }

        bool confirm_overwrite(const std::string& name) {
            if (!is_keytoken_name(name)) return true;
            std::cout << "Warning: keytoken '" << name << "' already exists. Overwrite it? (y/n): ";
            std::string line;
            std::getline(std::cin, line);
            return !line.empty() && (line[0] == 'y' || line[0] == 'Y');
        }

        void register_builtins() {
            auto add = [&](TokenSpec spec) {
                builtins_.emplace(spec.name, std::move(spec));
            };

            // Core executable primitives
            add(make_builtin("say", Kind::Output, 1, {"value"},
                "Prints a value.",
                [&](Runtime& rt, const std::vector<std::string>& args) {
                    std::cout << rt.resolve_value(args[0]).as_string() << "\n";
                }));

            add(make_builtin("get", Kind::Retrieve, 1, {"name"},
                "Pushes the value of a variable onto the stack.",
                [&](Runtime& rt, const std::vector<std::string>& args) {
                    const auto name = args[0];
                    auto it = rt.vars_.find(name);
                    rt.stack_.push_back(it == rt.vars_.end() ? Value{} : it->second);
                }));

            add(make_builtin("set", Kind::Assign, 2, {"name", "value"},
                "Stores a value in a variable.",
                [&](Runtime& rt, const std::vector<std::string>& args) {
                    const std::string& name = args[0];
                    if (rt.is_keytoken_name(name)) {
                        throw std::runtime_error("variable name conflicts with an existing keytoken: " + name);
                    }
                    rt.vars_[name] = rt.resolve_value(args[1]);
                }));

            add(make_builtin("make", Kind::Define, 3, {"name", "params", "definition"},
                "Creates a user token interactively or from inline parameters.",
                [&](Runtime& rt, const std::vector<std::string>& args) {
                    rt.create_custom_token_from_args(args);
                }));

            add(make_builtin("view", Kind::View, 1, {"name"},
                "Displays a built-in or custom token definition.",
                [&](Runtime& rt, const std::vector<std::string>& args) {
                    rt.view_token(args[0]);
                }));

            add(make_builtin("modify", Kind::Update, 1, {"name"},
                "Modifies a custom token.",
                [&](Runtime& rt, const std::vector<std::string>& args) {
                    rt.modify_token_interactive(args[0]);
                }));

            add(make_builtin("delete", Kind::Delete, 1, {"name"},
                "Deletes a custom token.",
                [&](Runtime& rt, const std::vector<std::string>& args) {
                    rt.delete_custom_token(args[0]);
                }));

            add(make_builtin("list", Kind::List, 0, {},
                "Lists all available tokens.",
                [&](Runtime& rt, const std::vector<std::string>&) {
                    rt.list_tokens();
                }));

            add(make_builtin("look", Kind::Autocomplete, 1, {"prefix"},
                "Shows completion candidates for a prefix.",
                [&](Runtime& rt, const std::vector<std::string>& args) {
                    rt.autocomplete(args[0]);
                }));

            add(make_builtin("use", Kind::IncludeFile, 1, {"path"},
                "Executes another source file.",
                [&](Runtime& rt, const std::vector<std::string>& args) {
                    rt.execute_file(fs::path(rt.resolve_value(args[0]).as_string()), rt.include_guard_);
                }));

            add(make_builtin("load", Kind::LoadFile, 1, {"path"},
                "Loads a file and reports success; it does not execute it.",
                [&](Runtime& rt, const std::vector<std::string>& args) {
                    const auto p = fs::path(rt.resolve_value(args[0]).as_string());
                    std::ifstream in(p.string());
                    if (!in) throw std::runtime_error("cannot load file: " + p.string());
                    std::cout << "Loaded file: " << p.string() << "\n";
                }));

            add(make_builtin("save", Kind::SaveFile, 1, {"path"},
                "Saves the current custom token database to a path.",
                [&](Runtime& rt, const std::vector<std::string>& args) {
                    rt.save_user_tokens(fs::path(rt.resolve_value(args[0]).as_string()));
                }));

            add(make_builtin("call", Kind::Call, 1, {"name"},
                "Calls a user token by name.",
                [&](Runtime& rt, const std::vector<std::string>& args) {
                    rt.execute_token(rt.resolve_value(args[0]).as_string(), {});
                }));

            add(make_builtin("if", Kind::Conditional, 3, {"condition", "then_token", "else_token"},
                "Branches to one of two tokens.",
                [&](Runtime& rt, const std::vector<std::string>& args) {
                    bool cond = rt.resolve_value(args[0]).as_bool();
                    rt.execute_token(cond ? args[1] : args[2], {});
                }));

            add(make_builtin("when", Kind::Conditional, 2, {"condition", "then_token"},
                "Executes a token when the condition is true.",
                [&](Runtime& rt, const std::vector<std::string>& args) {
                    if (rt.resolve_value(args[0]).as_bool()) rt.execute_token(args[1], {});
                }));

            add(make_builtin("while", Kind::Loop, 2, {"condition_token", "definition_token"},
                "Repeatedly executes definition while the condition token yields true.",
                [&](Runtime& rt, const std::vector<std::string>& args) {
                    for (std::size_t guard = 0; guard < 1000000; ++guard) {
                        rt.execute_token(args[0], {});
                        if (!rt.stack_.empty() && !rt.stack_.back().as_bool()) break;
                        rt.execute_token(args[1], {});
                    }
                }));

            add(make_builtin("up", Kind::StackDup, 0, {},
                "Duplicates the top of stack.",
                [&](Runtime& rt, const std::vector<std::string>&) {
                    if (rt.stack_.empty()) throw std::runtime_error("stack underflow on up");
                    rt.stack_.push_back(rt.stack_.back());
                }));

            add(make_builtin("out", Kind::StackPop, 0, {},
                "Drops the top of stack.",
                [&](Runtime& rt, const std::vector<std::string>&) {
                    if (rt.stack_.empty()) throw std::runtime_error("stack underflow on out");
                    rt.stack_.pop_back();
                }));

            add(make_builtin("back", Kind::Return, 0, {},
                "Returns by popping one stack frame marker if present.",
                [&](Runtime& rt, const std::vector<std::string>&) {
                    if (!rt.stack_.empty()) rt.stack_.pop_back();
                }));

            add(make_builtin("go", Kind::Call, 1, {"name"},
                "Alias for call.",
                [&](Runtime& rt, const std::vector<std::string>& args) {
                    rt.execute_token(rt.resolve_value(args[0]).as_string(), {});
                }));

            add(make_builtin("new", Kind::Define, 1, {"name"},
                "Creates a variable with empty value.",
                [&](Runtime& rt, const std::vector<std::string>& args) {
                    const std::string& name = args[0];
                    if (rt.is_keytoken_name(name)) {
                        throw std::runtime_error("variable name conflicts with an existing keytoken: " + name);
                    }
                    rt.vars_[name] = Value{};
                }));

            add(make_builtin("want", Kind::Input, 1, {"prompt"},
                "Reads a line from the user and pushes it onto the stack.",
                [&](Runtime& rt, const std::vector<std::string>& args) {
                    std::string prompt = rt.resolve_value(args[0]).as_string();
                    std::cout << prompt;
                    std::string line;
                    std::getline(std::cin, line);
                    rt.stack_.push_back(line);
                }));

            add(make_builtin("time", Kind::TimeQuery, 0, {},
                "Pushes the current UNIX timestamp.",
                [&](Runtime& rt, const std::vector<std::string>&) {
                    using namespace std::chrono;
                    auto now = system_clock::now().time_since_epoch();
                    auto secs = duration_cast<seconds>(now).count();
                    rt.stack_.push_back(static_cast<long long>(secs));
                }));

            add(make_builtin("now", Kind::TimeQuery, 0, {},
                "Pushes an ISO-like current time string.",
                [&](Runtime& rt, const std::vector<std::string>&) {
                    using namespace std::chrono;
                    auto t = system_clock::to_time_t(system_clock::now());
                    std::tm tm{};
    #if defined(_WIN32)
                    localtime_s(&tm, &t);
    #else
                    localtime_r(&t, &tm);
    #endif
                    std::ostringstream oss;
                    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
                    rt.stack_.push_back(oss.str());
                }));

            add(make_builtin("give", Kind::Arithmetic, 2, {"a", "b"},
                "Adds two numeric values.",
                [&](Runtime& rt, const std::vector<std::string>& args) {
                    rt.stack_.push_back(rt.resolve_value(args[0]).as_double() + rt.resolve_value(args[1]).as_double());
                }));

            add(make_builtin("take", Kind::Arithmetic, 2, {"a", "b"},
                "Subtracts the second numeric value from the first.",
                [&](Runtime& rt, const std::vector<std::string>& args) {
                    rt.stack_.push_back(rt.resolve_value(args[0]).as_double() - rt.resolve_value(args[1]).as_double());
                }));

            add(make_builtin("work", Kind::Arithmetic, 2, {"a", "b"},
                "Multiplies two numeric values.",
                [&](Runtime& rt, const std::vector<std::string>& args) {
                    rt.stack_.push_back(rt.resolve_value(args[0]).as_double() * rt.resolve_value(args[1]).as_double());
                }));

            add(make_builtin("divide", Kind::Arithmetic, 2, {"a", "b"},
                "Divides the first numeric value by the second.",
                [&](Runtime& rt, const std::vector<std::string>& args) {
                    const double denom = rt.resolve_value(args[1]).as_double();
                    if (denom == 0.0) throw std::runtime_error("division by zero");
                    rt.stack_.push_back(rt.resolve_value(args[0]).as_double() / denom);
                }));

            add(make_builtin("equal", Kind::Compare, 2, {"a", "b"},
                "Pushes whether two values are equal.",
                [&](Runtime& rt, const std::vector<std::string>& args) {
                    rt.stack_.push_back(rt.resolve_value(args[0]).as_string() == rt.resolve_value(args[1]).as_string());
                }));

            add(make_builtin("less", Kind::Compare, 2, {"a", "b"},
                "Pushes whether a < b.",
                [&](Runtime& rt, const std::vector<std::string>& args) {
                    rt.stack_.push_back(rt.resolve_value(args[0]).as_double() < rt.resolve_value(args[1]).as_double());
                }));

            add(make_builtin("more", Kind::Compare, 2, {"a", "b"},
                "Pushes whether a > b.",
                [&](Runtime& rt, const std::vector<std::string>& args) {
                    rt.stack_.push_back(rt.resolve_value(args[0]).as_double() > rt.resolve_value(args[1]).as_double());
                }));

            add(make_builtin("and", Kind::Logic, 2, {"a", "b"},
                "Logical conjunction.",
                [&](Runtime& rt, const std::vector<std::string>& args) {
                    rt.stack_.push_back(rt.resolve_value(args[0]).as_bool() && rt.resolve_value(args[1]).as_bool());
                }));

            add(make_builtin("or", Kind::Logic, 2, {"a", "b"},
                "Logical disjunction.",
                [&](Runtime& rt, const std::vector<std::string>& args) {
                    rt.stack_.push_back(rt.resolve_value(args[0]).as_bool() || rt.resolve_value(args[1]).as_bool());
                }));

            add(make_builtin("not", Kind::Logic, 1, {"value"},
                "Logical negation.",
                [&](Runtime& rt, const std::vector<std::string>& args) {
                    rt.stack_.push_back(!rt.resolve_value(args[0]).as_bool());
                }));

            add(make_builtin("help", Kind::NoOp, 0, {},
                "Shows help.",
                [&](Runtime& rt, const std::vector<std::string>&) {
                    rt.print_help();
                }));


            // Seed all common tokens from the attached list as keytokens.
            // Each token is now registered with its own semantic handler.
            const std::vector<std::string> seed = {
                "the", "be", "to", "of", "and", "a", "in", "that", "have", "I",
                "it", "for", "not", "on", "with", "he", "as", "you", "do", "at",
                "this", "but", "his", "by", "from", "they", "we", "say", "her", "she",
                "or", "an", "will", "my", "one", "all", "would", "there", "their", "what",
                "so", "up", "out", "if", "about", "who", "get", "which", "go", "me",
                "when", "make", "can", "like", "time", "no", "just", "him", "know", "take",
                "people", "into", "year", "your", "good", "some", "could", "them", "see", "other",
                "than", "then", "now", "look", "only", "come", "its", "over", "think", "also",
                "back", "after", "use", "two", "how", "our", "work", "first", "well", "way",
                "even", "new", "want", "because", "any", "these", "give", "day", "most", "us"
            };

            for (const auto& name : seed) {
                builtins_[name] = strict_seed_spec(name);
            }
        }

        static std::size_t default_arity(Kind kind, const std::string& name) {
            switch (kind) {
                case Kind::NoOp: return 0;
                case Kind::Output: return 1;
                case Kind::Input: return 1;
                case Kind::Query: return 1;
                case Kind::StackPush: return 1;
                case Kind::StackPop: return 0;
                case Kind::StackDup: return 0;
                case Kind::StackSwap: return 0;
                case Kind::Assign: return 2;
                case Kind::Retrieve: return 1;
                case Kind::Arithmetic: return 2;
                case Kind::Compare: return 2;
                case Kind::Logic: return kind == Kind::Logic && name == "not" ? 1 : 2;
                case Kind::Conditional: return (name == "when" ? 2 : 3);
                case Kind::Loop: return 2;
                case Kind::Call: return 1;
                case Kind::Return: return 0;
                case Kind::IncludeFile: return 1;
                case Kind::SaveFile: return 1;
                case Kind::LoadFile: return 1;
                case Kind::Autocomplete: return 1;
                case Kind::TimeQuery: return 0;
                default: return 0;
            }
        }

        static std::vector<std::string> default_param_names(Kind kind, std::size_t arity) {
            std::vector<std::string> out;
            for (std::size_t i = 0; i < arity; ++i) out.push_back("arg" + std::to_string(i + 1));
            switch (kind) {
                case Kind::Output: out = {"value"}; break;
                case Kind::Input: out = {"prompt"}; break;
                case Kind::Assign: out = {"name", "value"}; break;
                case Kind::Retrieve: out = {"name"}; break;
                case Kind::Arithmetic: out = {"a", "b"}; break;
                case Kind::Compare: out = {"a", "b"}; break;
                case Kind::Conditional: out = (arity == 2 ? std::vector<std::string>{"condition", "then_token"}
                                                           : std::vector<std::string>{"condition", "then_token", "else_token"}); break;
                case Kind::Loop: out = {"condition_token", "definition_token"}; break;
                case Kind::Call: out = {"name"}; break;
                case Kind::IncludeFile: out = {"path"}; break;
                case Kind::SaveFile: out = {"path"}; break;
                case Kind::LoadFile: out = {"path"}; break;
                case Kind::Autocomplete: out = {"prefix"}; break;
                case Kind::Query: out = {"topic"}; break;
                default: break;
            }
            return out;
        }

        static Handler default_handler_for(Kind kind, const std::string& name) {
            return [kind, name](Runtime& rt, const std::vector<std::string>& args) {
                switch (kind) {
                    case Kind::NoOp:
                        return;
                    case Kind::Output:
                        std::cout << rt.resolve_value(args.at(0)).as_string() << "\n";
                        return;
                    case Kind::Input: {
                        std::cout << rt.resolve_value(args.at(0)).as_string();
                        std::string line;
                        std::getline(std::cin, line);
                        rt.stack_.push_back(line);
                        return;
                    }
                    case Kind::Query:
                        rt.stack_.push_back(rt.query_about(name, rt.resolve_value(args.at(0)).as_string()));
                        return;
                    case Kind::Retrieve: {
                        auto it = rt.vars_.find(args.at(0));
                        rt.stack_.push_back(it == rt.vars_.end() ? Value{} : it->second);
                        return;
                    }
                    case Kind::Assign:
                        rt.vars_[args.at(0)] = rt.resolve_value(args.at(1));
                        return;
                    case Kind::Arithmetic: {
                        const double a = rt.resolve_value(args.at(0)).as_double();
                        const double b = rt.resolve_value(args.at(1)).as_double();
                        if (name == "take") rt.stack_.push_back(a - b);
                        else if (name == "work") rt.stack_.push_back(a * b);
                        else if (name == "give") rt.stack_.push_back(a + b);
                        else if (name == "divide") {
                            if (b == 0.0) throw std::runtime_error("division by zero");
                            rt.stack_.push_back(a / b);
                        } else rt.stack_.push_back(a + b);
                        return;
                    }
                    case Kind::Compare: {
                        const auto a = rt.resolve_value(args.at(0)).as_string();
                        const auto b = rt.resolve_value(args.at(1)).as_string();
                        if (name == "than") rt.stack_.push_back(rt.resolve_value(args.at(0)).as_double() > rt.resolve_value(args.at(1)).as_double());
                        else rt.stack_.push_back(a == b);
                        return;
                    }
                    case Kind::Logic: {
                        if (name == "not") rt.stack_.push_back(!rt.resolve_value(args.at(0)).as_bool());
                        else if (name == "and") rt.stack_.push_back(rt.resolve_value(args.at(0)).as_bool() && rt.resolve_value(args.at(1)).as_bool());
                        else if (name == "or" || name == "but") rt.stack_.push_back(rt.resolve_value(args.at(0)).as_bool() || rt.resolve_value(args.at(1)).as_bool());
                        else rt.stack_.push_back(rt.resolve_value(args.at(0)).as_bool());
                        return;
                    }
                    case Kind::Conditional:
                        if (name == "when" || name == "would" || name == "will" || name == "could") {
                            if (rt.resolve_value(args.at(0)).as_bool()) rt.execute_token(args.at(1), {});
                        } else {
                            bool cond = rt.resolve_value(args.at(0)).as_bool();
                            rt.execute_token(cond ? args.at(1) : args.at(2), {});
                        }
                        return;
                    case Kind::Loop:
                        rt.loop_token(args.at(0), args.at(1));
                        return;
                    case Kind::Call:
                        rt.execute_token(rt.resolve_value(args.at(0)).as_string(), {});
                        return;
                    case Kind::Return:
                        if (!rt.stack_.empty()) rt.stack_.pop_back();
                        return;
                    case Kind::IncludeFile:
                        rt.execute_file(fs::path(rt.resolve_value(args.at(0)).as_string()), rt.include_guard_);
                        return;
                    case Kind::SaveFile:
                        rt.save_user_tokens(fs::path(rt.resolve_value(args.at(0)).as_string()));
                        return;
                    case Kind::LoadFile:
                        rt.load_source_file(fs::path(rt.resolve_value(args.at(0)).as_string()));
                        return;
                    case Kind::Autocomplete:
                        rt.autocomplete(rt.resolve_value(args.at(0)).as_string());
                        return;
                    case Kind::TimeQuery:
                        rt.push_time(name);
                        return;
                    default:
                        return;
                }
            };
        }

        TokenSpec make_builtin(const std::string& name, Kind kind, std::size_t arity,
                               std::vector<std::string> params, std::string description, Handler h) {
            TokenSpec spec;
            spec.name = name;
            spec.kind = kind;
            spec.arity = arity;
            spec.param_names = std::move(params);
            spec.description = std::move(description);
            spec.handler = std::move(h);
            spec.builtin = true;
            return spec;
        }


        void push_text(const std::string& text) {
            stack_.emplace_back(text);
        }

        void push_bool_value(bool value) {
            stack_.emplace_back(value);
        }

        void push_int_value(long long value) {
            stack_.emplace_back(value);
        }

        std::string prompt_text(const std::string& prompt) {
            std::cout << prompt;
            std::string line;
            if (!std::getline(std::cin >> std::ws, line)) {
                throw std::runtime_error("input closed during prompt");
            }
            return trim(line);
        }

        std::vector<std::string> variable_candidates(const std::string& fragment) const {
            std::vector<std::string> hits;
            for (const auto& [name, _] : vars_) {
                if (name == fragment || starts_with(name, fragment) || starts_with(fragment, name) || name.find(fragment) != std::string::npos) {
                    hits.push_back(name);
                }
            }
            std::sort(hits.begin(), hits.end());
            hits.erase(std::unique(hits.begin(), hits.end()), hits.end());
            return hits;
        }

        std::string choose_candidate_name(const std::string& label, const std::vector<std::string>& candidates) {
            if (candidates.empty()) {
                return prompt_text(label + " refers to what? ");
            }
            if (candidates.size() == 1) {
                return candidates.front();
            }

            std::cout << "Ambiguous reference for '" << label << "'.\n";
            for (std::size_t i = 0; i < candidates.size(); ++i) {
                std::cout << (i + 1) << ") " << candidates[i] << '\n';
            }

            for (;;) {
                std::cout << "Choose 1-" << candidates.size() << ": ";
                std::string line;
                if (!std::getline(std::cin >> std::ws, line)) {
                    throw std::runtime_error("input closed during disambiguation");
                }
                std::istringstream iss(line);
                std::size_t choice = 0;
                char extra = '\0';
                if ((iss >> choice) && choice >= 1 && choice <= candidates.size() && !(iss >> extra)) {
                    return candidates[choice - 1];
                }
                std::cout << "Invalid choice.\n";
            }
        }

        std::string resolve_definite_reference(const std::string& article, const std::string& noun) {
            const auto candidates = variable_candidates(noun);
            if (!candidates.empty()) {
                const std::string chosen = choose_candidate_name(article + " " + noun, candidates);
                return vars_.at(chosen).as_string();
            }

            const std::string resolved = prompt_text("What exact referent does '" + article + " " + noun + "' denote? ");
            vars_["definite:" + noun] = resolved;
            return resolved;
        }

        std::string create_indefinite_reference(const std::string& article, const std::string& noun, bool vowel_required) {
            const std::string resolved = resolve_value(noun).as_string();
            const bool vowel = !resolved.empty() && std::string("aeiouAEIOU").find(resolved.front()) != std::string::npos;
            if (vowel_required && !vowel) {
                const std::string corrected = prompt_text("'" + article + " " + noun + "' is not a natural vowel-start form. Provide the intended form: ");
                vars_["indefinite:" + noun + ":" + std::to_string(vars_.size())] = corrected;
                return corrected;
            }
            const std::string key = "indefinite:" + noun + ":" + std::to_string(vars_.size());
            vars_[key] = resolved;
            return key;
        }

        std::string resolve_pronoun_reference(const std::string& pronoun, const std::vector<std::string>& args, bool possessive) {
            const std::string slot = "pronoun:" + pronoun;
            std::string referent;
            if (!args.empty()) {
                referent = resolve_value(args.front()).as_string();
                vars_[slot] = referent;
            } else if (auto it = vars_.find(slot); it != vars_.end()) {
                referent = it->second.as_string();
            } else {
                referent = prompt_text("Who or what does '" + pronoun + "' refer to? ");
                vars_[slot] = referent;
            }
            return possessive ? (referent + "'s") : referent;
        }

        std::string relation_string(const std::string& relation, const std::vector<std::string>& args) const {
            std::ostringstream oss;
            oss << relation << '(';
            for (std::size_t i = 0; i < args.size(); ++i) {
                if (i != 0) oss << ", ";
                oss << resolve_value(args[i]).as_string();
            }
            oss << ')';
            return oss.str();
        }

        std::string list_variable_names(const std::string& prefix = {}) const {
            std::vector<std::string> names;
            for (const auto& [name, _] : vars_) {
                if (prefix.empty() || starts_with(name, prefix)) {
                    names.push_back(name);
                }
            }
            std::sort(names.begin(), names.end());
            names.erase(std::unique(names.begin(), names.end()), names.end());
            return join_tokens(names);
        }

        void push_time_value(const std::string& granularity) {
            using namespace std::chrono;
            const auto now = system_clock::now();
            const auto tt = system_clock::to_time_t(now);
            std::tm tm = *std::localtime(&tt);

            if (granularity == "year") {
                stack_.emplace_back(static_cast<long long>(tm.tm_year + 1900));
                return;
            }
            if (granularity == "day") {
                stack_.emplace_back(static_cast<long long>(tm.tm_mday));
                return;
            }
            if (granularity == "now") {
                stack_.emplace_back(static_cast<long long>(tt));
                return;
            }

            std::ostringstream oss;
            oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
            stack_.emplace_back(oss.str());
        }


        TokenSpec strict_seed_spec(const std::string& name) {
            auto relation_spec = [&](Kind kind,
                                     std::size_t arity,
                                     std::vector<std::string> params,
                                     const std::string& description,
                                     const std::string& relation) -> TokenSpec {
                return make_builtin(name, kind, arity, std::move(params), description,
                    [relation](Runtime& rt, const std::vector<std::string>& args) {
                        rt.push_text(rt.relation_string(relation, args));
                    });
            };

            auto constant_spec = [&](Kind kind,
                                     const std::string& description,
                                     const std::string& value) -> TokenSpec {
                return make_builtin(name, kind, 0, {}, description,
                    [value](Runtime& rt, const std::vector<std::string>&) {
                        rt.push_text(value);
                    });
            };

            auto pronoun_spec = [&](const std::string& pronoun,
                                    bool possessive,
                                    std::size_t arity,
                                    std::vector<std::string> params,
                                    const std::string& description) -> TokenSpec {
                return make_builtin(name, Kind::Retrieve, arity, std::move(params), description,
                    [pronoun, possessive](Runtime& rt, const std::vector<std::string>& args) {
                        rt.push_text(rt.resolve_pronoun_reference(pronoun, args, possessive));
                    });
            };

            auto boolean_spec = [&](Kind kind,
                                    std::size_t arity,
                                    std::vector<std::string> params,
                                    const std::string& description,
                                    auto&& body) -> TokenSpec {
                return make_builtin(name, kind, arity, std::move(params), description,
                    [body](Runtime& rt, const std::vector<std::string>& args) {
                        body(rt, args);
                    });
            };

            auto arithmetic_spec = [&](const std::string& description,
                                       auto&& body) -> TokenSpec {
                return make_builtin(name, Kind::Arithmetic, 2, {"left", "right"}, description,
                    [body](Runtime& rt, const std::vector<std::string>& args) {
                        body(rt, args);
                    });
            };

            if (name == "the") {
                return make_builtin(name, Kind::Retrieve, 1, {"noun"}, "Resolves a definite referent and asks when ambiguous.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        rt.push_text(rt.resolve_definite_reference("the", rt.resolve_value(args.front()).as_string()));
                    });
            }
            if (name == "be") {
                return make_builtin(name, Kind::Assign, 2, {"name", "value"}, "Binds a name to a value and preserves the binding.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        const std::string key = rt.resolve_value(args[0]).as_string();
                        const Value value = rt.resolve_value(args[1]);
                        rt.vars_[key] = value;
                        rt.push_text(key + " = " + value.as_string());
                    });
            }
            if (name == "to") {
                return make_builtin(name, Kind::Call, 1, {"target"}, "Directs execution toward a named token.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        const std::string target = rt.resolve_value(args.front()).as_string();
                        rt.execute_token(target, {});
                        rt.push_text("to:" + target);
                    });
            }
            if (name == "of") return relation_spec(Kind::Define, 2, {"whole", "part"}, "Forms a part-whole relation.", "of");
            if (name == "and") {
                return boolean_spec(Kind::Logic, 2, {"left", "right"}, "Conjoins two values or booleans.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        const Value left = rt.resolve_value(args[0]);
                        const Value right = rt.resolve_value(args[1]);
                        if ((left.is_bool() || left.is_int() || left.is_double()) && (right.is_bool() || right.is_int() || right.is_double())) {
                            rt.push_bool_value(left.as_bool() && right.as_bool());
                        } else {
                            rt.push_text(left.as_string() + " and " + right.as_string());
                        }
                    });
            }
            if (name == "a") {
                return make_builtin(name, Kind::Define, 1, {"noun"}, "Creates a fresh indefinite referent.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        rt.push_text(rt.create_indefinite_reference("a", rt.resolve_value(args.front()).as_string(), false));
                    });
            }
            if (name == "in") return relation_spec(Kind::Compare, 2, {"item", "container"}, "Marks containment.", "in");
            if (name == "that") {
                return make_builtin(name, Kind::Retrieve, 1, {"noun"}, "Resolves a deictic reference.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        rt.push_text(rt.resolve_definite_reference("that", rt.resolve_value(args.front()).as_string()));
                    });
            }
            if (name == "have") {
                return make_builtin(name, Kind::Retrieve, 2, {"owner", "possession"}, "Stores or retrieves a possession relation.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        const std::string owner = rt.resolve_value(args[0]).as_string();
                        const std::string possession = rt.resolve_value(args[1]).as_string();
                        const std::string key = "have:" + owner + ":" + possession;
                        rt.vars_[key] = owner + " has " + possession;
                        rt.push_text(rt.vars_[key].as_string());
                    });
            }
            if (name == "I") return pronoun_spec("I", false, 0, {}, "Anchors the first-person singular referent.");
            if (name == "it") return pronoun_spec("it", false, 0, {}, "Anchors the neutral singular referent.");
            if (name == "for") return relation_spec(Kind::Define, 2, {"purpose", "thing"}, "Forms a purpose relation.", "for");
            if (name == "not") {
                return boolean_spec(Kind::Logic, 1, {"value"}, "Negates a boolean or prefixes a negation relation.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        const Value value = rt.resolve_value(args.front());
                        if (value.is_bool() || value.is_int() || value.is_double()) rt.push_bool_value(!value.as_bool());
                        else rt.push_text("not " + value.as_string());
                    });
            }
            if (name == "on") return relation_spec(Kind::Define, 2, {"surface", "thing"}, "Marks a surface relation.", "on");
            if (name == "with") return relation_spec(Kind::Define, 2, {"thing", "companion"}, "Marks accompaniment.", "with");
            if (name == "he") return pronoun_spec("he", false, 0, {}, "Anchors the masculine singular referent.");
            if (name == "as") return relation_spec(Kind::Define, 2, {"thing", "role"}, "Casts one thing as another role.", "as");
            if (name == "you") return pronoun_spec("you", false, 0, {}, "Anchors the second-person referent.");
            if (name == "do") {
                return make_builtin(name, Kind::Call, 1, {"action"}, "Executes a named action.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        const std::string action = rt.resolve_value(args.front()).as_string();
                        rt.execute_token(action, {});
                        rt.push_text("do:" + action);
                    });
            }
            if (name == "at") return relation_spec(Kind::Define, 2, {"place", "thing"}, "Marks a location relation.", "at");
            if (name == "this") return pronoun_spec("this", false, 1, {"noun"}, "Anchors the proximal demonstrative.");
            if (name == "but") {
                return boolean_spec(Kind::Logic, 2, {"left", "right"}, "Marks contrast between two values.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        const Value left = rt.resolve_value(args[0]);
                        const Value right = rt.resolve_value(args[1]);
                        if ((left.is_bool() || left.is_int() || left.is_double()) && (right.is_bool() || right.is_int() || right.is_double())) {
                            rt.push_bool_value(left.as_bool() && !right.as_bool());
                        } else {
                            rt.push_text(left.as_string() + " but " + right.as_string());
                        }
                    });
            }
            if (name == "his") return pronoun_spec("his", true, 0, {}, "Anchors the masculine possessive referent.");
            if (name == "by") return relation_spec(Kind::Define, 2, {"agent", "thing"}, "Marks an agentive relation.", "by");
            if (name == "from") {
                return make_builtin(name, Kind::IncludeFile, 1, {"source"}, "Extracts from a source or executes a source file when appropriate.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        const std::string source = rt.resolve_value(args.front()).as_string();
                        const fs::path path(source);
                        if (fs::exists(path)) {
                            rt.execute_file(path, rt.include_guard_);
                            rt.push_text("from:" + source);
                        } else {
                            auto it = rt.vars_.find(source);
                            rt.push_text(it == rt.vars_.end() ? source : it->second.as_string());
                        }
                    });
            }
            if (name == "they") return pronoun_spec("they", false, 0, {}, "Anchors the plural third-person referent.");
            if (name == "we") return pronoun_spec("we", false, 0, {}, "Anchors the first-person plural referent.");
            if (name == "say") {
                return make_builtin(name, Kind::Output, 1, {"message"}, "Prints a message and stores it as the last utterance.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        const std::string message = rt.resolve_value(args.front()).as_string();
                        std::cout << message << "\n";
                        rt.vars_["last_said"] = message;
                    });
            }
            if (name == "her") return pronoun_spec("her", true, 0, {}, "Anchors the feminine possessive referent.");
            if (name == "she") return pronoun_spec("she", false, 0, {}, "Anchors the feminine singular referent.");
            if (name == "or") {
                return boolean_spec(Kind::Logic, 2, {"left", "right"}, "Produces a logical or or a textual alternative.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        const Value left = rt.resolve_value(args[0]);
                        const Value right = rt.resolve_value(args[1]);
                        if ((left.is_bool() || left.is_int() || left.is_double()) && (right.is_bool() || right.is_int() || right.is_double())) {
                            rt.push_bool_value(left.as_bool() || right.as_bool());
                        } else {
                            rt.push_text(left.as_string() + " or " + right.as_string());
                        }
                    });
            }
            if (name == "an") {
                return make_builtin(name, Kind::Define, 1, {"noun"}, "Creates a vowel-sensitive indefinite referent.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        rt.push_text(rt.create_indefinite_reference("an", rt.resolve_value(args.front()).as_string(), true));
                    });
            }
            if (name == "will") {
                return make_builtin(name, Kind::Conditional, 1, {"action"}, "Commits to a named future action.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        const std::string action = rt.resolve_value(args.front()).as_string();
                        rt.vars_["will:" + action] = true;
                        rt.push_text("will:" + action);
                    });
            }
            if (name == "my") return pronoun_spec("my", true, 0, {}, "Anchors the first-person possessive referent.");
            if (name == "one") return constant_spec(Kind::Define, "The numeral one.", "1");
            if (name == "all") {
                return make_builtin(name, Kind::List, 0, {}, "Lists all known variable names.",
                    [](Runtime& rt, const std::vector<std::string>&) {
                        rt.push_text(rt.list_variable_names());
                    });
            }
            if (name == "would") {
                return make_builtin(name, Kind::Conditional, 2, {"condition", "action"}, "Represents a hypothetical branch.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        if (rt.resolve_value(args[0]).as_bool()) {
                            rt.execute_token(rt.resolve_value(args[1]).as_string(), {});
                        } else {
                            rt.push_text("would(" + rt.resolve_value(args[0]).as_string() + ", " + rt.resolve_value(args[1]).as_string() + ")");
                        }
                    });
            }
            if (name == "there") {
                return make_builtin(name, Kind::Query, 1, {"topic"}, "Checks existence of a topic in the current context.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        const std::string topic = rt.resolve_value(args.front()).as_string();
                        rt.push_bool_value(!rt.variable_candidates(topic).empty());
                    });
            }
            if (name == "their") return pronoun_spec("their", true, 0, {}, "Anchors the plural possessive referent.");
            if (name == "what") {
                return make_builtin(name, Kind::Query, 1, {"topic"}, "Asks for the identity of a topic.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        const std::string topic = rt.resolve_value(args.front()).as_string();
                        rt.push_text(rt.prompt_text("What is " + topic + "? "));
                    });
            }
            if (name == "so") return relation_spec(Kind::Conditional, 2, {"cause", "result"}, "Marks a causal consequence.", "so");
            if (name == "up") {
                return make_builtin(name, Kind::StackDup, 0, {}, "Duplicates the top stack value.",
                    [](Runtime& rt, const std::vector<std::string>&) {
                        if (rt.stack_.empty()) throw std::runtime_error("stack underflow on up");
                        rt.stack_.push_back(rt.stack_.back());
                    });
            }
            if (name == "out") {
                return make_builtin(name, Kind::StackPop, 0, {}, "Removes the top stack value.",
                    [](Runtime& rt, const std::vector<std::string>&) {
                        if (rt.stack_.empty()) throw std::runtime_error("stack underflow on out");
                        rt.stack_.pop_back();
                    });
            }
            if (name == "if") {
                return make_builtin(name, Kind::Conditional, 3, {"condition", "then_token", "else_token"}, "Branches on a boolean condition.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        const bool cond = rt.resolve_value(args[0]).as_bool();
                        rt.execute_token(cond ? rt.resolve_value(args[1]).as_string() : rt.resolve_value(args[2]).as_string(), {});
                    });
            }
            if (name == "about") {
                return make_builtin(name, Kind::Query, 1, {"topic"}, "Requests a short description of a topic.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        rt.push_text(rt.prompt_text("About " + rt.resolve_value(args.front()).as_string() + ": "));
                    });
            }
            if (name == "who") {
                return make_builtin(name, Kind::Query, 1, {"topic"}, "Requests a person-oriented answer.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        rt.push_text(rt.prompt_text("Who is " + rt.resolve_value(args.front()).as_string() + "? "));
                    });
            }
            if (name == "get") {
                return make_builtin(name, Kind::Retrieve, 1, {"name"}, "Retrieves a variable value.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        const std::string key = rt.resolve_value(args.front()).as_string();
                        auto it = rt.vars_.find(key);
                        rt.push_text(it == rt.vars_.end() ? std::string{} : it->second.as_string());
                    });
            }
            if (name == "which") {
                return make_builtin(name, Kind::Query, 1, {"topic"}, "Chooses among matching names when ambiguous.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        const std::string topic = rt.resolve_value(args.front()).as_string();
                        const auto candidates = rt.variable_candidates(topic);
                        const std::string selected = rt.choose_candidate_name("which " + topic, candidates);
                        rt.push_text(selected);
                    });
            }
            if (name == "go") {
                return make_builtin(name, Kind::Call, 1, {"target"}, "Executes a named target and returns its label.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        const std::string target = rt.resolve_value(args.front()).as_string();
                        rt.execute_token(target, {});
                        rt.push_text("go:" + target);
                    });
            }
            if (name == "me") return pronoun_spec("me", false, 0, {}, "Anchors the first-person object referent.");
            if (name == "when") {
                return make_builtin(name, Kind::Conditional, 2, {"condition", "then_token"}, "Executes a token when a condition is true.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        if (rt.resolve_value(args[0]).as_bool()) {
                            rt.execute_token(rt.resolve_value(args[1]).as_string(), {});
                        }
                    });
            }
            if (name == "make") {
                return make_builtin(name, Kind::Define, 3, {"name", "params", "definition"}, "Creates a custom token.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        rt.create_custom_token_from_args(args);
                    });
            }
            if (name == "can") {
                return make_builtin(name, Kind::Query, 1, {"topic"}, "Checks whether a token or variable is available.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        const std::string topic = rt.resolve_value(args.front()).as_string();
                        rt.push_bool_value(rt.is_keytoken_name(topic) || rt.is_variable_name(topic));
                    });
            }
            if (name == "like") {
                return make_builtin(name, Kind::Compare, 2, {"left", "right"}, "Checks similarity or equality.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        const Value left = rt.resolve_value(args[0]);
                        const Value right = rt.resolve_value(args[1]);
                        if ((left.is_int() || left.is_double()) && (right.is_int() || right.is_double())) {
                            rt.push_bool_value(left.as_double() == right.as_double());
                        } else {
                            const auto a = left.as_string();
                            const auto b = right.as_string();
                            rt.push_bool_value(a == b || a.find(b) != std::string::npos || b.find(a) != std::string::npos);
                        }
                    });
            }
            if (name == "time") return make_builtin(name, Kind::TimeQuery, 0, {}, "Pushes the current timestamp as text.",
                [](Runtime& rt, const std::vector<std::string>&) { rt.push_time_value("time"); });
            if (name == "no") return constant_spec(Kind::Logic, "The boolean false.", "false");
            if (name == "just") {
                return make_builtin(name, Kind::CustomMacro, 1, {"value"}, "Normalizes a value without adding anything extra.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        rt.push_text(trim(rt.resolve_value(args.front()).as_string()));
                    });
            }
            if (name == "him") return pronoun_spec("him", false, 0, {}, "Anchors the masculine object referent.");
            if (name == "know") {
                return make_builtin(name, Kind::Query, 1, {"topic"}, "Checks whether a topic is already known.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        const std::string topic = rt.resolve_value(args.front()).as_string();
                        rt.push_bool_value(rt.is_keytoken_name(topic) || rt.is_variable_name(topic));
                    });
            }
            if (name == "take") {
                return arithmetic_spec("Subtracts the right value from the left.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        const double left = rt.resolve_value(args[0]).as_double();
                        const double right = rt.resolve_value(args[1]).as_double();
                        rt.push_text(std::to_string(left - right));
                    });
            }
            if (name == "people") {
                return make_builtin(name, Kind::Query, 0, {}, "Lists known person-like referents.",
                    [](Runtime& rt, const std::vector<std::string>&) {
                        rt.push_text(rt.list_variable_names("pronoun:"));
                    });
            }
            if (name == "into") return relation_spec(Kind::Define, 2, {"source", "target"}, "Marks a transition into a target.", "into");
            if (name == "year") return make_builtin(name, Kind::TimeQuery, 0, {}, "Pushes the current year.",
                [](Runtime& rt, const std::vector<std::string>&) { rt.push_time_value("year"); });
            if (name == "your") return pronoun_spec("your", true, 0, {}, "Anchors the second-person possessive referent.");
            if (name == "good") {
                return make_builtin(name, Kind::Compare, 1, {"value"}, "Checks whether a value is acceptable.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        const std::string value = rt.resolve_value(args.front()).as_string();
                        rt.push_bool_value(!value.empty() && value != "0" && value != "false");
                    });
            }
            if (name == "some") {
                return make_builtin(name, Kind::Query, 1, {"topic"}, "Selects or prompts for one example.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        const std::string topic = rt.resolve_value(args.front()).as_string();
                        const auto candidates = rt.variable_candidates(topic);
                        if (!candidates.empty()) {
                            rt.push_text(rt.vars_.at(candidates.front()).as_string());
                        } else {
                            rt.push_text(rt.prompt_text("Give some example of " + topic + ": "));
                        }
                    });
            }
            if (name == "could") {
                return make_builtin(name, Kind::Conditional, 2, {"condition", "action"}, "Represents a possible action.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        if (rt.resolve_value(args[0]).as_bool()) {
                            rt.execute_token(rt.resolve_value(args[1]).as_string(), {});
                        } else {
                            rt.push_text("could(" + rt.resolve_value(args[0]).as_string() + ", " + rt.resolve_value(args[1]).as_string() + ")");
                        }
                    });
            }
            if (name == "them") return pronoun_spec("them", false, 0, {}, "Anchors the plural object referent.");
            if (name == "see") {
                return make_builtin(name, Kind::Query, 1, {"topic"}, "Inspects a value without consuming it.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        const std::string topic = rt.resolve_value(args.front()).as_string();
                        std::cout << topic << "\n";
                        rt.push_text(topic);
                    });
            }
            if (name == "other") {
                return make_builtin(name, Kind::Compare, 1, {"value"}, "Selects an alternative form.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        const std::string value = rt.resolve_value(args.front()).as_string();
                        rt.push_text("other(" + value + ")");
                    });
            }
            if (name == "than") {
                return make_builtin(name, Kind::Compare, 2, {"left", "right"}, "Compares two values numerically.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        rt.push_bool_value(rt.resolve_value(args[0]).as_double() > rt.resolve_value(args[1]).as_double());
                    });
            }
            if (name == "then") {
                return make_builtin(name, Kind::Conditional, 2, {"condition", "action"}, "Runs the action after the condition is established.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        if (rt.resolve_value(args[0]).as_bool()) rt.execute_token(rt.resolve_value(args[1]).as_string(), {});
                    });
            }
            if (name == "now") return make_builtin(name, Kind::TimeQuery, 0, {}, "Pushes the current epoch time.",
                [](Runtime& rt, const std::vector<std::string>&) { rt.push_time_value("now"); });
            if (name == "look") {
                return make_builtin(name, Kind::Autocomplete, 1, {"prefix"}, "Shows completion candidates.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        rt.autocomplete(rt.resolve_value(args.front()).as_string());
                    });
            }
            if (name == "only") {
                return make_builtin(name, Kind::Compare, 1, {"value"}, "Keeps only the selected value.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        rt.push_text(rt.resolve_value(args.front()).as_string());
                    });
            }
            if (name == "come") {
                return make_builtin(name, Kind::Call, 1, {"target"}, "Executes a target and then marks the return.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        const std::string target = rt.resolve_value(args.front()).as_string();
                        rt.execute_token(target, {});
                        rt.push_text("come:" + target);
                    });
            }
            if (name == "its") return pronoun_spec("its", true, 0, {}, "Anchors the neuter possessive referent.");
            if (name == "over") return relation_spec(Kind::Compare, 2, {"upper", "lower"}, "Marks an over/above relation.", "over");
            if (name == "think") {
                return make_builtin(name, Kind::Query, 1, {"topic"}, "Forms or asks for a thought about a topic.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        rt.push_text(rt.prompt_text("What do you think about " + rt.resolve_value(args.front()).as_string() + "? "));
                    });
            }
            if (name == "also") {
                return make_builtin(name, Kind::List, 1, {"value"}, "Appends an additional idea.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        const std::string value = rt.resolve_value(args.front()).as_string();
                        rt.push_text(value + " also");
                    });
            }
            if (name == "back") {
                return make_builtin(name, Kind::Return, 0, {}, "Undoes one stack step.",
                    [](Runtime& rt, const std::vector<std::string>&) {
                        if (!rt.stack_.empty()) rt.stack_.pop_back();
                    });
            }
            if (name == "after") return relation_spec(Kind::Conditional, 2, {"first", "second"}, "Marks temporal succession.", "after");
            if (name == "use") {
                return make_builtin(name, Kind::IncludeFile, 1, {"path"}, "Executes a source file.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        rt.execute_file(fs::path(rt.resolve_value(args.front()).as_string()), rt.include_guard_);
                    });
            }
            if (name == "two") return constant_spec(Kind::Define, "The numeral two.", "2");
            if (name == "how") {
                return make_builtin(name, Kind::Query, 1, {"topic"}, "Asks for a method or manner.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        rt.push_text(rt.prompt_text("How is " + rt.resolve_value(args.front()).as_string() + " done? "));
                    });
            }
            if (name == "our") return pronoun_spec("our", true, 0, {}, "Anchors the first-person plural possessive referent.");
            if (name == "work") {
                return arithmetic_spec("Multiplies two values.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        const double left = rt.resolve_value(args[0]).as_double();
                        const double right = rt.resolve_value(args[1]).as_double();
                        rt.push_text(std::to_string(left * right));
                    });
            }
            if (name == "first") {
                return make_builtin(name, Kind::Define, 1, {"value"}, "Selects or marks the first element.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        rt.push_text("first(" + rt.resolve_value(args.front()).as_string() + ")");
                    });
            }
            if (name == "well") {
                return make_builtin(name, Kind::Define, 1, {"value"}, "Normalizes a value into a well-formed form.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        rt.push_text(trim(rt.resolve_value(args.front()).as_string()));
                    });
            }
            if (name == "way") return relation_spec(Kind::Define, 2, {"path", "destination"}, "Marks a route or path.", "way");
            if (name == "even") {
                return make_builtin(name, Kind::Compare, 1, {"number"}, "Tests parity.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        const long long value = rt.resolve_value(args.front()).as_int();
                        rt.push_bool_value((value % 2) == 0);
                    });
            }
            if (name == "new") {
                return make_builtin(name, Kind::Define, 1, {"name"}, "Creates a fresh named referent.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        const std::string value = rt.resolve_value(args.front()).as_string();
                        const std::string key = "new:" + value + ":" + std::to_string(rt.vars_.size());
                        rt.vars_[key] = value;
                        rt.push_text(key);
                    });
            }
            if (name == "want") {
                return make_builtin(name, Kind::Input, 1, {"prompt"}, "Requests a desired value from the user.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        rt.push_text(rt.prompt_text(rt.resolve_value(args.front()).as_string()));
                    });
            }
            if (name == "because") return relation_spec(Kind::Conditional, 2, {"cause", "reason"}, "Marks a causal explanation.", "because");
            if (name == "any") {
                return make_builtin(name, Kind::Query, 1, {"topic"}, "Checks whether any matching referent exists.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        const std::string topic = rt.resolve_value(args.front()).as_string();
                        rt.push_bool_value(!rt.variable_candidates(topic).empty());
                    });
            }
            if (name == "these") return pronoun_spec("these", false, 1, {"noun"}, "Anchors the proximal plural referent.");
            if (name == "give") {
                return arithmetic_spec("Adds two values.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        const double left = rt.resolve_value(args[0]).as_double();
                        const double right = rt.resolve_value(args[1]).as_double();
                        rt.push_text(std::to_string(left + right));
                    });
            }
            if (name == "day") return make_builtin(name, Kind::TimeQuery, 0, {}, "Pushes the current day of the month.",
                [](Runtime& rt, const std::vector<std::string>&) { rt.push_time_value("day"); });
            if (name == "most") {
                return make_builtin(name, Kind::Query, 1, {"topic"}, "Selects the strongest available match.",
                    [](Runtime& rt, const std::vector<std::string>& args) {
                        const std::string topic = rt.resolve_value(args.front()).as_string();
                        const auto candidates = rt.variable_candidates(topic);
                        rt.push_text(candidates.empty() ? topic : candidates.back());
                    });
            }
            if (name == "us") return pronoun_spec("us", false, 0, {}, "Anchors the first-person plural object referent.");

            return make_builtin(name, Kind::CustomMacro, 0, {}, "Strict seeded token with default fallback semantics.",
                [](Runtime& rt, const std::vector<std::string>& args) {
                    if (!args.empty()) rt.push_text(rt.relation_string("token", args));
                });
        }

        Value resolve_value(const std::string& token) const {
            if (token.empty()) return {};
            if (token == "true") return true;
            if (token == "false") return false;
            if (token.size() > 2 && token.front() == '"' && token.back() == '"') return token.substr(1, token.size() - 2);
            bool is_num = !token.empty();
            bool has_dot = false;
            std::size_t i = 0;
            if (token[0] == '-' || token[0] == '+') i = 1;
            for (; i < token.size(); ++i) {
                if (token[i] == '.') {
                    if (has_dot) { is_num = false; break; }
                    has_dot = true;
                } else if (!std::isdigit(static_cast<unsigned char>(token[i]))) {
                    is_num = false;
                    break;
                }
            }
            if (is_num) {
                try {
                    if (has_dot) return std::stod(token);
                    return std::stoll(token);
                } catch (...) {
                    return token;
                }
            }
            if (auto it = vars_.find(token); it != vars_.end()) return it->second;
            return token;
        }

        void push_time(const std::string&) {
            using namespace std::chrono;
            auto now = system_clock::now().time_since_epoch();
            auto secs = duration_cast<seconds>(now).count();
            stack_.push_back(static_cast<long long>(secs));
        }

        std::string query_about(const std::string& token, const std::string& topic) {
            // Meaningful question-and-answer hook. In a larger build, this would
            // consult a knowledge base or a semantic definition table.
            std::cout << "[" << token << "] Please specify a value for \"" << topic << "\": ";
            std::string answer;
            std::getline(std::cin, answer);
            return answer;
        }

        void loop_token(const std::string& cond_token, const std::string& definition_token) {
            for (std::size_t guard = 0; guard < 1000000; ++guard) {
                execute_token(cond_token, {});
                if (stack_.empty()) break;
                if (!stack_.back().as_bool()) break;
                stack_.pop_back();
                execute_token(definition_token, {});
            }
        }

        void handle_meta_command(const std::string& line) {
            auto toks = split_ws(line);
            if (toks.empty()) return;
            const std::string cmd = toks[0];
            if (cmd == ":help") {
                print_help();
                return;
            }
            if (cmd == ":list") {
                list_tokens();
                return;
            }
            if (cmd == ":load") {
                if (toks.size() < 2) throw std::runtime_error("usage: :load <file>");
                const fs::path p(toks[1]);
                if (p.string() == "CEnglish.db") {
                    load_user_tokens(p);
                } else if (has_suffix(p.string(), ".ce")) {
                    load_source_code(p);
                } else {
                    load_source_file(p);
                }
                return;
            }
            if (cmd == ":save") {
                if (toks.size() < 2) throw std::runtime_error("usage: :save <file>");
                save_user_tokens(fs::path(toks[1]));
                return;
            }
            if (cmd == ":save_ce") {
                if (toks.size() < 2) throw std::runtime_error("usage: :save_ce <file.ce>");
                save_source_code(fs::path(toks[1]));
                return;
            }
            if (cmd == ":make") {
                create_custom_token_interactive();
                return;
            }
            if (cmd == ":view") {
                if (toks.size() < 2) throw std::runtime_error("usage: :view <token>");
                view_token(toks[1]);
                return;
            }
            if (cmd == ":modify") {
                if (toks.size() < 2) throw std::runtime_error("usage: :modify <token>");
                modify_token_interactive(toks[1]);
                return;
            }
            if (cmd == ":delete") {
                if (toks.size() < 2) throw std::runtime_error("usage: :delete <token>");
                delete_custom_token(toks[1]);
                return;
            }
            if (cmd == ":autocomplete") {
                if (toks.size() < 2) throw std::runtime_error("usage: :autocomplete <prefix>");
                autocomplete(toks[1]);
                return;
            }
            if (cmd == ":compile") {
                if (toks.size() < 3) throw std::runtime_error("usage: :compile <input> <output>");
                compile_source_to_file(fs::path(toks[1]), fs::path(toks[2]));
                return;
            }
            throw std::runtime_error("unknown meta-command: " + cmd);
        }

        void print_help() const {
            std::cout
                << "Meta-commands:\n"
                << "  :help\n  :list\n  :load <file>\n  :save <file>\n  :savece <file.ce>\n  :make <token>\n"
                << "  :view <token>\n  :modify <token>\n  :delete <token>\n"
                << "  :autocomplete <prefix>\n  :compile <input> <output>\n"
                << "CEnglish keywords include the 100 common words in English.\n";
        }

        void list_tokens() const {
            std::unordered_set<std::string> uniq;
            std::vector<std::string> names;
            names.reserve(builtins_.size() + custom_.size());
            for (const auto& [k, _] : builtins_) {
                if (uniq.insert(k).second) names.push_back(k);
            }
            for (const auto& [k, _] : custom_) {
                if (uniq.insert(k).second) names.push_back(k);
            }
            std::sort(names.begin(), names.end());
            for (const auto& n : names) std::cout << n << "\n";
        }

        void autocomplete(const std::string& prefix) const {
            std::unordered_set<std::string> seen;
            std::vector<std::pair<std::size_t, std::string>> hits;
            auto push_hit = [&](const std::string& s) {
                if (!starts_with(s, prefix)) return;
                if (seen.insert(s).second) hits.emplace_back(s.size(), s);
            };
            for (const auto& [k, _] : custom_) push_hit(k);
            for (const auto& [k, _] : builtins_) push_hit(k);
            std::sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) {
                if (a.first != b.first) return a.first < b.first;
                return a.second < b.second;
            });
            for (const auto& [_, s] : hits) std::cout << s << "\n";
        }

        void view_token(const std::string& name) const {
            const auto cit = custom_.find(name);
            const auto bit = builtins_.find(name);

            if (cit != custom_.end()) {
                const auto& t = cit->second;
                std::cout << "[custom] " << t.name;
                if (bit != builtins_.end()) std::cout << " (overrides builtin)";
                std::cout << "\n";
                std::cout << "  params:";
                for (const auto& p : t.param_names) std::cout << ' ' << p;
                std::cout << "\n  definition: " << join_tokens(t.definition_tokens) << "\n";
                std::cout << "  description: " << t.description << "\n";
                return;
            }
            if (bit != builtins_.end()) {
                const auto& t = bit->second;
                std::cout << "[builtin] " << t.name << "\n";
                std::cout << "  arity: " << t.arity << "\n";
                std::cout << "  params:";
                for (const auto& p : t.param_names) std::cout << ' ' << p;
                std::cout << "\n  description: " << t.description << "\n";
                return;
            }
            auto suggestions = suggest_tokens(name);
            std::cout << name << " not found.\n";
            if (!suggestions.empty()) {
                std::cout << "Token suggestions:\n";
                for (const auto& s : suggestions) std::cout << "  " << s << "\n";
            }
        }

        void create_custom_token_interactive(const std::string& forced_name = {}) {
            CustomToken tok;
            if (!forced_name.empty()) {
                tok.name = trim(forced_name);
                std::cout << "Name: " << tok.name << "\n";
            } else {
                std::cout << "Name: ";
                std::getline(std::cin, tok.name);
                tok.name = trim(tok.name);
            }
            if (tok.name.empty()) throw std::runtime_error("empty token name");

            if (!confirm_overwrite(tok.name)) {
                std::cout << "Skipped token: " << tok.name << "\n";
                return;
            }

            std::cout << "Parameter names (space-separated, blank for none): ";
            std::string line;
            std::getline(std::cin, line);
            tok.param_names = split_ws(trim(line));
            ensure_param_names_valid(tok.param_names, tok.name);

            std::cout << "Description: ";
            std::getline(std::cin, tok.description);
            std::cout << "Definition tokens (end with only QED on a line):\n";
            while (true) {
                std::getline(std::cin, line);
                if (trim(line) == "QED") break;
                auto row = split_ws(line);
                tok.definition_tokens.insert(tok.definition_tokens.end(), row.begin(), row.end());
            }

            custom_[tok.name] = std::move(tok);
            save_user_tokens(default_db_path());
        }

        void create_custom_token_from_args(const std::vector<std::string>& args) {
            if (args.empty()) {
                create_custom_token_interactive();
                return;
            }
            CustomToken tok;
            tok.name = args[0];
            if (tok.name.empty()) throw std::runtime_error("empty token name");
            if (!confirm_overwrite(tok.name)) {
                std::cout << "Skipped token: " << tok.name << "\n";
                return;
            }
            if (args.size() >= 2) {
                tok.param_names = split_csv(args[1]);
            }
            ensure_param_names_valid(tok.param_names, tok.name);
            if (args.size() >= 3) {
                tok.definition_tokens = split_ws(args[2]);
            } else {
                std::cout << "definition tokens for " << tok.name << " (end with a single QED on a line):\n";
                std::string line;
                while (std::getline(std::cin, line)) {
                    if (trim(line) == "QED") break;
                    auto row = split_ws(line);
                    tok.definition_tokens.insert(tok.definition_tokens.end(), row.begin(), row.end());
                }
            }
            custom_[tok.name] = std::move(tok);
            save_user_tokens(default_db_path());
        }

        static std::vector<std::string> split_csv(const std::string& s) {
            std::vector<std::string> out;
            std::string cur;
            for (char ch : s) {
                if (ch == ',') {
                    if (!trim(cur).empty()) out.push_back(trim(cur));
                    cur.clear();
                } else {
                    cur.push_back(ch);
                }
            }
            if (!trim(cur).empty()) out.push_back(trim(cur));
            return out;
        }

        void modify_token_interactive(const std::string& name) {
            if (!is_keytoken_name(name)) throw std::runtime_error("token not found: " + name);
            if (!confirm_overwrite(name)) {
                std::cout << "Skipped token: " << name << "\n";
                return;
            }

            CustomToken tok;
            if (auto it = custom_.find(name); it != custom_.end()) {
                tok = it->second;
            } else {
                tok.name = name;
                tok.description.clear();
                tok.definition_tokens.clear();
            }
            tok.name = name;

            std::cout << "New parameter names (space-separated, blank to keep): ";
            std::string line;
            std::getline(std::cin, line);
            line = trim(line);
            if (!line.empty()) tok.param_names = split_ws(line);
            ensure_param_names_valid(tok.param_names, tok.name);

            if (has_builtin(name)) {
                std::cout << "Modifying builtin token '" << name << "'; the user-defined version will take precedence.\n";
            }

            std::cout << "Replace definition? (y/n): ";
            std::getline(std::cin, line);
            if (!line.empty() && (line[0] == 'y' || line[0] == 'Y')) {
                tok.definition_tokens.clear();
                std::cout << "New definition tokens (end with a single QED on a line):\n";
                while (true) {
                    std::getline(std::cin, line);
                    if (trim(line) == "QED") break;
                    auto row = split_ws(line);
                    tok.definition_tokens.insert(tok.definition_tokens.end(), row.begin(), row.end());
                }
            }
            custom_[name] = std::move(tok);
            save_user_tokens(default_db_path());
        }

        void delete_custom_token(const std::string& name) {
            auto it = custom_.find(name);
            if (it == custom_.end()) throw std::runtime_error("custom token not found: " + name);
            custom_.erase(it);
            save_user_tokens(default_db_path());
            std::cout << "Deleted " << name << "\n";
        }

        void execute_line(const std::string& line, const std::string& source_name) {
            auto toks = split_ws(line);
            if (toks.empty()) return;
            execute_tokens(toks, source_name);
        }

        static bool has_suffix(const std::string& s, const std::string& suffix) {
            return s.size() >= suffix.size() &&
                   s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
        }

        const CustomToken* find_custom(const std::string& name) const {
            if (auto it = custom_.find(name); it != custom_.end()) return &it->second;
            return nullptr;
        }

        void execute_tokens(const std::vector<std::string>& toks, const std::string& source_name) {
            struct PendingCall {
                std::string token;
                bool builtin{false};
                std::size_t arity{0};
                std::vector<std::string> param_names;
                std::vector<std::string> args;
            };

            auto is_keytoken = [&](const std::string& s) -> bool {
                return find_spec(s) != nullptr || find_custom(s) != nullptr;
            };

            auto collect_side = [&](std::size_t i, int dir, const std::vector<bool>& consumed) {
                std::vector<std::size_t> idx;
                if (dir < 0) {
                    for (std::size_t j = i; j > 0;) {
                        --j;
                        if (consumed[j] || is_keytoken(toks[j])) break;
                        idx.push_back(j);
                    }
                    std::reverse(idx.begin(), idx.end());
                } else {
                    for (std::size_t j = i + 1; j < toks.size(); ++j) {
                        if (consumed[j] || is_keytoken(toks[j])) break;
                        idx.push_back(j);
                    }
                }
                return idx;
            };

            auto enumerate_combinations =
                [&](auto&& self,
                    const std::vector<std::size_t>& pool,
                    std::size_t start,
                    std::size_t need,
                    std::vector<std::size_t>& current,
                    std::vector<std::vector<std::size_t>>& out) -> void
            {
                if (need == 0) {
                    out.push_back(current);
                    return;
                }
                if (start >= pool.size()) {
                    return;
                }

                for (std::size_t i = start; i + need <= pool.size(); ++i) {
                    current.push_back(pool[i]);
                    self(self, pool, i + 1, need - 1, current, out);
                    current.pop_back();
                }
            };

            auto format_option = [&](const std::string& token,
                                     const std::vector<std::size_t>& indices,
                                     const std::vector<std::string>& param_names) -> std::string
            {
                std::ostringstream oss;
                oss << token << '(';
                for (std::size_t k = 0; k < indices.size(); ++k) {
                    if (k != 0) oss << ", ";
                    if (k < param_names.size() && !param_names[k].empty()) {
                        oss << param_names[k] << '=';
                    }
                    oss << toks[indices[k]];
                }
                oss << ')';
                return oss.str();
            };

            auto choose_disambiguation = [&](const std::string& token,
                                             const std::vector<std::vector<std::size_t>>& options,
                                             const std::vector<std::string>& param_names) -> std::size_t
            {
                if (options.size() <= 1) return 0;

                std::cout << "Ambiguous use of '" << token << "' in " << source_name << ".\n";
                for (std::size_t i = 0; i < options.size(); ++i) {
                    std::cout << (i + 1) << ") " << format_option(token, options[i], param_names) << '\n';
                }

                for (;;) {
                    std::cout << "Choose 1-" << options.size() << ": ";
                    std::string line;
                    if (!std::getline(std::cin >> std::ws, line)) {
                        throw std::runtime_error("input closed during disambiguation");
                    }

                    std::istringstream iss(line);
                    std::size_t choice = 0;
                    char extra = '\0';
                    if ((iss >> choice) && choice >= 1 && choice <= options.size() && !(iss >> extra)) {
                        return choice - 1;
                    }

                    std::cout << "Invalid choice.\n";
                }
            };

            std::vector<bool> consumed(toks.size(), false);
            std::vector<PendingCall> calls;
            calls.reserve(toks.size());

            for (std::size_t i = 0; i < toks.size(); ++i) {
                if (consumed[i]) continue;

                const std::string& tok = toks[i];
                if (tok.empty()) continue;

                const CustomToken* custom = find_custom(tok);
                const TokenSpec* spec = custom ? nullptr : find_spec(tok);

                if (!spec && !custom) {
                    if (is_variable_name(tok)) continue;
                    if (prompt_make_or_skip(tok, source_name)) {
                        consumed[i] = true;
                    }
                    continue;
                }

                PendingCall call;
                call.token = tok;

                if (spec) {
                    call.builtin = true;
                    call.arity = spec->arity;
                    call.param_names = spec->param_names;
                } else {
                    call.builtin = false;
                    call.arity = custom->param_names.size();
                    call.param_names = custom->param_names;
                }

                std::vector<std::size_t> pool;
                {
                    const auto left = collect_side(i, -1, consumed);
                    const auto right = collect_side(i, +1, consumed);
                    pool.reserve(left.size() + right.size());
                    pool.insert(pool.end(), left.begin(), left.end());
                    pool.insert(pool.end(), right.begin(), right.end());
                }

                std::vector<std::size_t> selected;

                if (call.arity == 0) {
                    selected.clear();
                } else if (pool.empty()) {
                    selected.clear();
                } else {
                    const std::size_t take = std::min(call.arity, pool.size());

                    std::vector<std::vector<std::size_t>> options;
                    options.reserve(8);

                    std::vector<std::size_t> current;
                    current.reserve(take);
                    enumerate_combinations(enumerate_combinations, pool, 0, take, current, options);

                    if (options.empty()) {
                        selected.clear();
                    } else if (options.size() == 1) {
                        selected = std::move(options.front());
                    } else {
                        const std::size_t choice = choose_disambiguation(call.token, options, call.param_names);
                        selected = std::move(options[choice]);
                    }
                }

                for (std::size_t idx : selected) {
                    consumed[idx] = true;
                }

                call.args.reserve(selected.size());
                for (std::size_t idx : selected) {
                    call.args.push_back(toks[idx]);
                }

                calls.push_back(std::move(call));
            }

            for (std::size_t i = 0; i < toks.size(); ++i) {
                if (consumed[i]) continue;
                if (is_keytoken_name(toks[i]) || is_variable_name(toks[i])) continue;
                if (prompt_make_or_skip(toks[i], source_name)) {
                    consumed[i] = true;
                    continue;
                }
            }

            for (const auto& call : calls) {
                if (call.builtin) {
                    const TokenSpec* spec = find_spec(call.token);
                    if (!spec) {
                        throw std::runtime_error("unknown token '" + call.token + "'");
                    }

                    std::vector<std::string> final_args = call.args;
                    if (final_args.size() < spec->arity) {
                        for (std::size_t i = final_args.size(); i < spec->arity; ++i) {
                            final_args.push_back(prompt_for_parameter(call.token, spec->param_names, i));
                        }
                    }

                    spec->handler(*this, final_args);
                } else {
                    const CustomToken* custom = find_custom(call.token);
                    if (!custom) {
                        throw std::runtime_error("unknown token '" + call.token + "'");
                    }

                    std::vector<std::string> final_args = call.args;
                    if (final_args.size() < custom->param_names.size()) {
                        for (std::size_t i = final_args.size(); i < custom->param_names.size(); ++i) {
                            final_args.push_back(prompt_for_parameter(call.token, custom->param_names, i));
                        }
                    }

                    expand_and_execute_custom(*custom, final_args);
                }
            }
        }

        void execute_token(const std::string& tok, const std::vector<std::string>& args) {
            if (auto it = custom_.find(tok); it != custom_.end()) {
                expand_and_execute_custom(it->second, args);
                return;
            }
            const TokenSpec* spec = find_spec(tok);
            if (!spec) {
                if (is_variable_name(tok)) return;
                if (prompt_make_or_skip(tok, "<direct>")) return;
                return;
            }
            std::vector<std::string> final_args = args;
            if (final_args.size() < spec->arity) {
                for (std::size_t i = final_args.size(); i < spec->arity; ++i) {
                    final_args.push_back(prompt_for_parameter(tok, spec->param_names, i));
                }
            }
            spec->handler(*this, final_args);
        }

        const TokenSpec* find_spec(const std::string& name) const {
            if (auto it = builtins_.find(name); it != builtins_.end()) return &it->second;
            return nullptr;
        }

        std::vector<std::string> suggest_tokens(const std::string& name) const {
            std::unordered_set<std::string> seen;
            std::vector<std::pair<long long, std::string>> scored;
            auto score = [&](const std::string& s) {
                return edit_distance(name, s);
            };
            for (const auto& [k, _] : custom_) {
                if (seen.insert(k).second) scored.emplace_back(score(k), k);
            }
            for (const auto& [k, _] : builtins_) {
                if (seen.insert(k).second) scored.emplace_back(score(k), k);
            }
            std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) {
                if (a.first != b.first) return a.first < b.first;
                return a.second < b.second;
            });
            std::vector<std::string> out;
            for (std::size_t i = 0; i < std::min<std::size_t>(5, scored.size()); ++i) out.push_back(scored[i].second);
            return out;
        }

        bool prompt_make_or_skip(const std::string& tok, const std::string& source_name) {
            std::cout << "Unknown token '" << tok << "' in " << source_name << ". Define/make token now? (y/n): ";
            std::string line;
            if (!std::getline(std::cin, line)) {
                return false;
            }
            if (!line.empty() && (line[0] == 'y' || line[0] == 'Y')) {
                create_custom_token_interactive(tok);
                return true;
            }
            std::cout << "Skipped token: " << tok << "\n";
            return true;
        }

        std::string prompt_for_parameter(const std::string& token,
                                         const std::vector<std::string>& params,
                                         std::size_t index) {
            std::string prompt;
            if (index < params.size()) prompt = params[index];
            else prompt = "arg" + std::to_string(index + 1);
            std::cout << "[" << token << "] enter " << prompt << ": ";
            std::string line;
            std::getline(std::cin, line);
            return trim(line);
        }

        void expand_and_execute_custom(const CustomToken& tok, const std::vector<std::string>& args) {
            std::unordered_map<std::string, std::string> binding;
            for (std::size_t i = 0; i < tok.param_names.size(); ++i) {
                const std::string& name = tok.param_names[i];
                binding[name] = (i < args.size() ? args[i] : prompt_for_parameter(tok.name, tok.param_names, i));
            }
            std::vector<std::string> expanded;
            expanded.reserve(tok.definition_tokens.size());
            for (std::string t : tok.definition_tokens) {
                for (const auto& [k, v] : binding) replace_all(t, "{" + k + "}", v);
                expanded.push_back(std::move(t));
            }
            execute_tokens(expanded, "<custom:" + tok.name + ">");
        }

        static void replace_all(std::string& s, const std::string& from, const std::string& to) {
            if (from.empty()) return;
            std::size_t pos = 0;
            while ((pos = s.find(from, pos)) != std::string::npos) {
                s.replace(pos, from.size(), to);
                pos += to.size();
            }
        }

        void save_source_code(const fs::path& path) const {
            std::ofstream out(path.string(), std::ios::trunc);
            if (!out) throw std::runtime_error("cannot open source file for writing: " + path.string());
            for (const auto& [name, tok] : custom_) {
                out << "===KEYTOKEN:" << tok.name << "===\n";
                if (!tok.param_names.empty()) {
                    out << "===PARAMS:";
                    for (std::size_t i = 0; i < tok.param_names.size(); ++i) {
                        if (i) out << ',';
                        out << tok.param_names[i] << '=';
                    }
                    out << "===\n";
                }
                for (const auto& t : tok.definition_tokens) out << t << ' ';
                out << "\n===END===\n";
            }
            std::cout << "Saved source code to " << path.string() << "\n";
        }

        void save_user_tokens(const fs::path& path) const {
            std::ofstream out(path.string(), std::ios::trunc);
            if (!out) throw std::runtime_error("cannot open database for writing: " + path.string());
            for (const auto& [name, tok] : custom_) {
                out << "===KEYTOKEN:" << tok.name << "===\n";
                if (!tok.param_names.empty()) {
                    out << "===PARAMS:";
                    for (std::size_t i = 0; i < tok.param_names.size(); ++i) {
                        if (i) out << ',';
                        out << tok.param_names[i] << '=';
                    }
                    out << "===\n";
                }
                if (!tok.description.empty()) out << "# " << tok.description << "\n";
                for (const auto& t : tok.definition_tokens) out << t << ' ';
                out << "\n===END===\n";
            }
            std::cout << "Saved custom token to " << path.string() << "\n";
        }

        void load_user_tokens(const fs::path& path) {
            if (!fs::exists(path)) return;
            std::ifstream in(path.string());
            if (!in) return;
            custom_.clear();
            std::string line;
            CustomToken current;
            bool in_block = false;
            while (std::getline(in, line)) {
                line = trim(line);
                if (line.rfind("===KEYTOKEN:", 0) == 0 && line.size() >= 12) {
                    current = CustomToken{};
                    in_block = true;
                    current.name = line.substr(11, line.size() - 14);
                    continue;
                }
                if (!in_block) continue;
                if (line.rfind("===PARAMS:", 0) == 0) {
                    auto definition = line.substr(10, line.size() - 13);
                    current.param_names.clear();
                    for (auto& item : split_csv(definition)) {
                        auto eq = item.find('=');
                        current.param_names.push_back(eq == std::string::npos ? trim(item) : trim(item.substr(0, eq)));
                    }
                    continue;
                }
                if (line.rfind("#", 0) == 0) {
                    current.description = trim(line.substr(1));
                    continue;
                }
                if (line == "===END===") {
                    if (!current.name.empty()) {
                        ensure_param_names_valid(current.param_names, current.name);
                        custom_[current.name] = current;
                    }
                    in_block = false;
                    continue;
                }
                auto toks = split_ws(line);
                current.definition_tokens.insert(current.definition_tokens.end(), toks.begin(), toks.end());
            }
        }

         void load_source_file(const fs::path& path) {
            if (!fs::exists(path)) throw std::runtime_error("cannot load file: " + path.string());
            std::ifstream in(path.string());
            if (!in) throw std::runtime_error("cannot open file: " + path.string());
            std::string line;
            while (std::getline(in, line)) execute_line(line, path.string());
        }

        void load_source_code(const fs::path& path) {
            load_user_tokens(path);
        }

        void execute_file(const fs::path& path, std::unordered_set<std::string>& guard) {
            fs::path abs = fs::absolute(path);
            const std::string key = abs.string();
            if (guard.count(key)) throw std::runtime_error("recursive include detected: " + key);
            guard.insert(key);
            include_stack_.push_back(abs);
            std::ifstream in(abs.string());
            if (!in) {
                include_stack_.pop_back();
                guard.erase(key);
                throw std::runtime_error("cannot open file: " + abs.string());
            }
            std::string line;
            try {
                while (std::getline(in, line)) {
                    const auto tokens = split_ws(line);
                    if (!tokens.empty() && tokens[0] == "use" && tokens.size() >= 2) {
                        execute_token("use", {tokens[1]});
                    } else if (!tokens.empty()) {
                        execute_tokens(tokens, abs.string());
                    }
                }
            } catch (...) {
                include_stack_.pop_back();
                guard.erase(key);
                throw;
            }
            include_stack_.pop_back();
            guard.erase(key);
        }

        void compile_source_to_file(const fs::path& input, const fs::path& output) {
            std::ifstream in(input.string());
            if (!in) throw std::runtime_error("cannot open input file: " + input.string());
            std::ofstream out(output.string(), std::ios::trunc);
            if (!out) throw std::runtime_error("cannot open output file: " + output.string());
            std::string line;
            std::size_t line_no = 0;
            while (std::getline(in, line)) {
                ++line_no;
                auto toks = split_ws(line);
                if (toks.empty()) continue;
                for (const auto& t : toks) {
                    if (!find_spec(t) && custom_.find(t) == custom_.end()) {
                        auto sug = suggest_tokens(t);
                        out << "# warning line " << line_no << ": unknown token " << t;
                        if (!sug.empty()) {
                            out << " ; suggestions:";
                            for (const auto& s : sug) out << ' ' << s;
                        }
                        out << "\n";
                    }
                }
                out << line << "\n";
            }
            std::cout << "Compiled " << input.string() << " -> " << output.string() << "\n";
        }
    };

}

int main(int argc, char** argv) {
    CEnglish::Runtime rt;
    while (true) {
        try {
            if (argc >= 2) {
                return rt.run_file(fs::path(argv[1]));
            }
            rt.repl();
        } catch (const std::exception& e) {
            std::cerr << "Fatal error: " << e.what() << "\n";
        }
    }
    return 0;
}
