// CEnglish.cpp
// C++17, standard-library only.
//
// A practical single-file runtime for a token-based English-keyword language.
// It provides:
// - case-sensitive keytokens
// - built-in token registry seeded from the 100-word common-word list
// - interactive parameter questioning when arguments are omitted
// - user-defined token create/view/modify/delete
// - disk persistence for user-defined tokens
// - autocomplete suggestions
// - diagnostics with edit-distance hints
// - multi-file loading via `use <file>`
// - a small stack/variable VM for executable semantics

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

namespace fs {
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
} // namespace fs

namespace CEnglish {

    using Word = std::string;
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
        Word name;
        Kind kind{Kind::NoOp};
        std::size_t arity{0};
        std::vector<std::string> param_names;
        std::string description;
        Handler handler;
        bool builtin{false};
    };

    struct CustomToken {
        Word name;
        std::vector<std::string> param_names;
        std::vector<std::string> body_tokens;
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
            std::cout << "CEnglish programming language. Type :help for commands.\n";
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
                execute_file(path, stack);
            } catch (const std::exception& e) {
                std::cerr << "Runtime error: " << e.what() << "\n";
                return 1;
            }
            return 0;
        }

    private:
        std::unordered_map<Word, TokenSpec> builtins_;
        std::unordered_map<Word, CustomToken> custom_;
        std::unordered_map<Word, Value> vars_;
        std::vector<Value> stack_;
        std::vector<Diagnostic> diagnostics_;
        std::vector<fs::path> include_stack_;
        std::unordered_set<std::string> include_guard_;

        static fs::path default_db_path() {
            return fs::path("CEnglish.db");
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
                    rt.vars_[args[0]] = rt.resolve_value(args[1]);
                }));

            add(make_builtin("make", Kind::Define, 3, {"name", "params", "body"},
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
                    rt.modify_custom_token(args[0]);
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

            add(make_builtin("while", Kind::Loop, 2, {"condition_token", "body_token"},
                "Repeatedly executes body while the condition token yields true.",
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
                    rt.vars_[args[0]] = Value{};
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

            // Seed all common words from the attached list as keytokens.
            // Most map to semantic aliases or no-ops; the registry still treats every word as a valid keytoken.
            const std::vector<std::pair<std::string, Kind>> seed = {
                {"the", Kind::NoOp}, {"be", Kind::Assign}, {"to", Kind::Call}, {"of", Kind::NoOp},
                {"and", Kind::Logic}, {"a", Kind::NoOp}, {"in", Kind::NoOp}, {"that", Kind::NoOp},
                {"have", Kind::Retrieve}, {"I", Kind::Retrieve}, {"it", Kind::Retrieve}, {"for", Kind::NoOp},
                {"not", Kind::Logic}, {"on", Kind::NoOp}, {"with", Kind::NoOp}, {"he", Kind::Retrieve},
                {"as", Kind::NoOp}, {"you", Kind::Retrieve}, {"do", Kind::Call}, {"at", Kind::NoOp},
                {"this", Kind::NoOp}, {"but", Kind::Logic}, {"his", Kind::Retrieve}, {"by", Kind::NoOp},
                {"from", Kind::IncludeFile}, {"they", Kind::Retrieve}, {"we", Kind::Retrieve}, {"say", Kind::Output},
                {"her", Kind::Retrieve}, {"she", Kind::Retrieve}, {"or", Kind::Logic}, {"an", Kind::NoOp},
                {"will", Kind::Conditional}, {"my", Kind::Retrieve}, {"one", Kind::Define}, {"all", Kind::List},
                {"would", Kind::Conditional}, {"there", Kind::Query}, {"their", Kind::Retrieve}, {"what", Kind::Query},
                {"so", Kind::NoOp}, {"up", Kind::StackDup}, {"out", Kind::StackPop}, {"if", Kind::Conditional},
                {"about", Kind::Query}, {"who", Kind::Query}, {"get", Kind::Retrieve}, {"which", Kind::Query},
                {"go", Kind::Call}, {"me", Kind::Retrieve}, {"when", Kind::Conditional}, {"make", Kind::Define},
                {"can", Kind::Query}, {"like", Kind::Query}, {"time", Kind::TimeQuery}, {"no", Kind::Logic},
                {"just", Kind::NoOp}, {"him", Kind::Retrieve}, {"know", Kind::Query}, {"take", Kind::Arithmetic},
                {"people", Kind::Query}, {"into", Kind::IncludeFile}, {"year", Kind::TimeQuery}, {"your", Kind::Retrieve},
                {"good", Kind::NoOp}, {"some", Kind::Query}, {"could", Kind::Conditional}, {"them", Kind::Retrieve},
                {"see", Kind::Query}, {"other", Kind::NoOp}, {"than", Kind::Compare}, {"then", Kind::Conditional},
                {"now", Kind::TimeQuery}, {"look", Kind::Autocomplete}, {"only", Kind::NoOp}, {"come", Kind::Call},
                {"its", Kind::Retrieve}, {"over", Kind::NoOp}, {"think", Kind::Query}, {"also", Kind::NoOp},
                {"back", Kind::Return}, {"after", Kind::NoOp}, {"use", Kind::IncludeFile}, {"two", Kind::Define},
                {"how", Kind::Query}, {"our", Kind::Retrieve}, {"work", Kind::Arithmetic}, {"first", Kind::Define},
                {"well", Kind::NoOp}, {"way", Kind::NoOp}, {"even", Kind::NoOp}, {"new", Kind::Define},
                {"want", Kind::Input}, {"because", Kind::NoOp}, {"any", Kind::Query}, {"these", Kind::Retrieve},
                {"give", Kind::Arithmetic}, {"day", Kind::TimeQuery}, {"most", Kind::Query}, {"us", Kind::Retrieve}
            };

            for (const auto& [name, kind] : seed) {
                if (builtins_.find(name) != builtins_.end()) continue;
                TokenSpec spec;
                spec.name = name;
                spec.kind = kind;
                spec.builtin = true;
                spec.description = "Seeded common-word keytoken.";
                spec.arity = default_arity(kind, name);
                spec.param_names = default_param_names(kind, spec.arity);
                spec.handler = default_handler_for(kind, name);
                builtins_.emplace(spec.name, std::move(spec));
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
                case Kind::Loop: out = {"condition_token", "body_token"}; break;
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

        void loop_token(const std::string& cond_token, const std::string& body_token) {
            for (std::size_t guard = 0; guard < 1000000; ++guard) {
                execute_token(cond_token, {});
                if (stack_.empty()) break;
                if (!stack_.back().as_bool()) break;
                stack_.pop_back();
                execute_token(body_token, {});
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
                modify_custom_token(toks[1]);
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
                << "  :help\n  :list\n  :load <file>\n  :save <file>\n  :make <token>\n"
                << "  :view <token>\n  :modify <token>\n  :delete <token>\n"
                << "  :autocomplete <prefix>\n  :compile <input> <output>\n"
                << "Language tokens include the 100 common words plus technical primitives.\n";
        }

        void list_tokens() const {
            std::vector<std::string> names;
            names.reserve(builtins_.size() + custom_.size());
            for (const auto& [k, _] : builtins_) names.push_back(k);
            for (const auto& [k, _] : custom_) names.push_back(k);
            std::sort(names.begin(), names.end());
            for (const auto& n : names) std::cout << n << "\n";
        }

        void autocomplete(const std::string& prefix) const {
            std::vector<std::pair<std::size_t, std::string>> hits;
            auto push_hit = [&](const std::string& s) {
                if (starts_with(s, prefix)) hits.emplace_back(s.size(), s);
            };
            for (const auto& [k, _] : builtins_) push_hit(k);
            for (const auto& [k, _] : custom_) push_hit(k);
            std::sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) {
                if (a.first != b.first) return a.first < b.first;
                return a.second < b.second;
            });
            for (const auto& [_, s] : hits) std::cout << s << "\n";
        }

        void view_token(const std::string& name) const {
            if (auto it = builtins_.find(name); it != builtins_.end()) {
                const auto& t = it->second;
                std::cout << "[builtin] " << t.name << "\n";
                std::cout << "  arity: " << t.arity << "\n";
                std::cout << "  params:";
                for (const auto& p : t.param_names) std::cout << ' ' << p;
                std::cout << "\n  description: " << t.description << "\n";
                return;
            }
            if (auto it = custom_.find(name); it != custom_.end()) {
                const auto& t = it->second;
                std::cout << "[custom] " << t.name << "\n";
                std::cout << "  params:";
                for (const auto& p : t.param_names) std::cout << ' ' << p;
                std::cout << "\n  body: " << join_tokens(t.body_tokens) << "\n";
                std::cout << "  description: " << t.description << "\n";
                return;
            }
            auto suggestions = suggest_tokens(name);
            std::cout << "Token not found: " << name << "\n";
            if (!suggestions.empty()) {
                std::cout << "Did you mean:\n";
                for (const auto& s : suggestions) std::cout << "  " << s << "\n";
            }
        }

        void create_custom_token_interactive() {
            CustomToken tok;
            std::cout << "Name: ";
            std::getline(std::cin, tok.name);
            tok.name = trim(tok.name);
            if (tok.name.empty()) throw std::runtime_error("empty token name");
            std::cout << "Parameter names (space-separated, blank for none): ";
            std::string line;
            std::getline(std::cin, line);
            tok.param_names = split_ws(trim(line));
            std::cout << "Description: ";
            std::getline(std::cin, tok.description);
            std::cout << "Definition tokens (end with only QED on a line):\n";
            while (true) {
                std::getline(std::cin, line);
                if (trim(line) == "QED") break;
                auto row = split_ws(line);
                tok.body_tokens.insert(tok.body_tokens.end(), row.begin(), row.end());
            }
            custom_[tok.name] = tok;
            save_user_tokens(default_db_path());
            std::cout << "Created token: " << tok.name << "\n";
        }

        void create_custom_token_from_args(const std::vector<std::string>& args) {
            if (args.empty()) {
                create_custom_token_interactive();
                return;
            }
            CustomToken tok;
            tok.name = args[0];
            if (args.size() >= 2) {
                // convention: second argument may be a comma-separated param list
                tok.param_names = split_csv(args[1]);
            }
            if (args.size() >= 3) {
                tok.body_tokens = split_ws(args[2]);
            } else {
                std::cout << "Body tokens for " << tok.name << " (end with a single . on a line):\n";
                std::string line;
                while (std::getline(std::cin, line)) {
                    if (trim(line) == ".") break;
                    auto row = split_ws(line);
                    tok.body_tokens.insert(tok.body_tokens.end(), row.begin(), row.end());
                }
            }
            custom_[tok.name] = tok;
            save_user_tokens(default_db_path());
            std::cout << "Created token: " << tok.name << "\n";
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

        void modify_custom_token(const std::string& name) {
            auto it = custom_.find(name);
            if (it == custom_.end()) throw std::runtime_error("custom token not found: " + name);
            std::cout << "New parameter names (space-separated, blank to keep): ";
            std::string line;
            std::getline(std::cin, line);
            line = trim(line);
            if (!line.empty()) it->second.param_names = split_ws(line);
            std::cout << "Replace body? (y/n): ";
            std::getline(std::cin, line);
            if (!line.empty() && (line[0] == 'y' || line[0] == 'Y')) {
                it->second.body_tokens.clear();
                std::cout << "New body tokens (end with a single . on a line):\n";
                while (true) {
                    std::getline(std::cin, line);
                    if (trim(line) == ".") break;
                    auto row = split_ws(line);
                    it->second.body_tokens.insert(it->second.body_tokens.end(), row.begin(), row.end());
                }
            }
            save_user_tokens(default_db_path());
        }

        void delete_custom_token(const std::string& name) {
            auto it = custom_.find(name);
            if (it == custom_.end()) throw std::runtime_error("custom token not found: " + name);
            custom_.erase(it);
            save_user_tokens(default_db_path());
            std::cout << "Deleted token: " << name << "\n";
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

                const TokenSpec* spec = find_spec(tok);
                const CustomToken* custom = spec ? nullptr : find_custom(tok);

                if (!spec && !custom) continue;

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
                if (is_keytoken(toks[i])) continue;

                auto suggestions = suggest_tokens(toks[i]);
                std::ostringstream oss;
                oss << "unknown token '" << toks[i] << "' in " << source_name;
                if (!suggestions.empty()) {
                    oss << ". suggestions:";
                    for (const auto& s : suggestions) oss << ' ' << s;
                }
                throw std::runtime_error(oss.str());
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
            const TokenSpec* spec = find_spec(tok);
            if (!spec) {
                auto it = custom_.find(tok);
                if (it != custom_.end()) {
                    expand_and_execute_custom(it->second, args);
                    return;
                }
                auto suggestions = suggest_tokens(tok);
                std::ostringstream oss;
                oss << "unknown token '" << tok << "'";
                if (!suggestions.empty()) {
                    oss << ". suggestions:";
                    for (const auto& s : suggestions) oss << ' ' << s;
                }
                throw std::runtime_error(oss.str());
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
            std::vector<std::pair<long long, std::string>> scored;
            auto score = [&](const std::string& s) {
                return edit_distance(name, s);
            };
            for (const auto& [k, _] : builtins_) scored.emplace_back(score(k), k);
            for (const auto& [k, _] : custom_) scored.emplace_back(score(k), k);
            std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) {
                if (a.first != b.first) return a.first < b.first;
                return a.second < b.second;
            });
            std::vector<std::string> out;
            for (std::size_t i = 0; i < std::min<std::size_t>(5, scored.size()); ++i) out.push_back(scored[i].second);
            return out;
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
            expanded.reserve(tok.body_tokens.size());
            for (std::string t : tok.body_tokens) {
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

        void save_user_tokens(const fs::path& path) const {
            std::ofstream out(path.string(), std::ios::trunc);
            if (!out) throw std::runtime_error("cannot open database for writing: " + path.string());
            for (const auto& [name, tok] : custom_) {
                out << "===KEYWORD:" << tok.name << "===\n";
                if (!tok.param_names.empty()) {
                    out << "===PARAMS:";
                    for (std::size_t i = 0; i < tok.param_names.size(); ++i) {
                        if (i) out << ',';
                        out << tok.param_names[i] << '=';
                    }
                    out << "===\n";
                }
                if (!tok.description.empty()) out << "# " << tok.description << "\n";
                for (const auto& t : tok.body_tokens) out << t << ' ';
                out << "\n===END===\n";
            }
            std::cout << "Saved custom tokens to " << path.string() << "\n";
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
                if (line.rfind("===KEYWORD:", 0) == 0 && line.size() >= 12) {
                    current = CustomToken{};
                    in_block = true;
                    current.name = line.substr(11, line.size() - 14);
                    continue;
                }
                if (!in_block) continue;
                if (line.rfind("===PARAMS:", 0) == 0) {
                    auto body = line.substr(10, line.size() - 13);
                    current.param_names.clear();
                    for (auto& item : split_csv(body)) {
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
                    if (!current.name.empty()) custom_[current.name] = current;
                    in_block = false;
                    continue;
                }
                auto toks = split_ws(line);
                current.body_tokens.insert(current.body_tokens.end(), toks.begin(), toks.end());
            }
        }

        void load_source_file(const fs::path& path) {
            if (!fs::exists(path)) throw std::runtime_error("cannot load file: " + path.string());
            std::ifstream in(path.string());
            if (!in) throw std::runtime_error("cannot open file: " + path.string());
            std::string line;
            while (std::getline(in, line)) execute_line(line, path.string());
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
    }; // close class Runtime

} // close namespace CEnglish

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
