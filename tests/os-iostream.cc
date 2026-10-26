/*
 * C++ iostreams / localization conformance test for the slimmed OSv unikernel.
 *
 * The rest of the test suite prints through C <cstdio> (printf). This one
 * exercises the C++ streams layer specifically - std::cout / std::cerr,
 * std::endl, the formatting manipulators and the in-memory string streams -
 * because all of that lives behind libc++'s localization support (the locale
 * facets: ctype, num_put/num_get, numpunct). That support is built against
 * llvm-libc's plain POSIX locale interface (newlocale/uselocale + the *_l
 * functions), with a single static "C"/US locale; there is no glibc or musl
 * locale machinery underneath.
 *
 * Output goes to the console; the checks additionally format into
 * std::ostringstream and parse back via std::istringstream so the result is
 * verified in-program rather than by eyeballing the log. os_iostream_main()
 * returns non-zero if any check fails.
 */
#include <iostream>
#include <sstream>
#include <iomanip>
#include <string>
#include <vector>
#include <locale>

static int g_checks = 0;
static int g_fails  = 0;

#define ICHECK(cond)                                                          \
    do {                                                                      \
        ++g_checks;                                                           \
        if (!(cond)) {                                                        \
            ++g_fails;                                                        \
            std::cerr << "    FAIL " << __FILE__ << ':' << __LINE__ << ": "   \
                      << #cond << '\n';                                       \
        }                                                                     \
    } while (0)

// Round-trip a value through operator<< and compare against the expectation.
template <class T>
static std::string fmt(const T &value)
{
    std::ostringstream os;
    os << value;
    return os.str();
}

int os_iostream_main()
{
    std::cout << "OSv C++ iostreams & localization test\n";
    std::cout << "=====================================" << std::endl;

    // The headline: cout / endl actually reach the console.
    std::cout << "Hello from std::cout, std::endl and friends!" << std::endl;
    std::cerr << "(this line is std::cerr)" << std::endl;

    std::cout << "\n== basic insertion ==\n";
    ICHECK(fmt(42) == "42");
    ICHECK(fmt(-7) == "-7");
    ICHECK(fmt(std::string("text")) == "text");
    ICHECK(fmt('Z') == "Z");
    ICHECK(fmt(3.14) == "3.14");
    ICHECK(fmt(true) == "1");

    std::cout << "== numeric base manipulators ==\n";
    {
        std::ostringstream os;
        os << std::hex << 255 << ' ' << std::oct << 8 << ' ' << std::dec << 9;
        ICHECK(os.str() == "ff 10 9");

        std::ostringstream os2;
        os2 << std::showbase << std::hex << 255;
        ICHECK(os2.str() == "0xff");
    }

    std::cout << "== width / fill / alignment ==\n";
    {
        std::ostringstream os;
        os << std::setw(5) << std::setfill('0') << 42;
        ICHECK(os.str() == "00042");

        std::ostringstream os2;
        os2 << std::left << std::setw(4) << 7 << '|';
        ICHECK(os2.str() == "7   |");
    }

    std::cout << "== floating point formatting ==\n";
    {
        std::ostringstream os;
        os << std::fixed << std::setprecision(3) << 2.5;
        ICHECK(os.str() == "2.500");

        std::ostringstream os2;
        os2 << std::scientific << std::setprecision(2) << 12345.0;
        ICHECK(os2.str() == "1.23e+04");
    }

    std::cout << "== boolalpha ==\n";
    {
        std::ostringstream os;
        os << std::boolalpha << true << ' ' << false;
        ICHECK(os.str() == "true false");
    }

    std::cout << "== istringstream parsing (num_get facet) ==\n";
    {
        std::istringstream is("123 45.5 hello 0xff");
        int i = 0; double d = 0; std::string w;
        is >> i >> d >> w;
        ICHECK(i == 123);
        ICHECK(d == 45.5);
        ICHECK(w == "hello");

        unsigned u = 0;
        is >> std::hex >> u;       // consume "0xff"
        ICHECK(u == 0xff);

        std::istringstream is2("3.14159");
        double pi = 0; is2 >> pi;
        ICHECK(pi > 3.14 && pi < 3.1416);   // decimal point came from "C" locale
    }

    std::cout << "== getline over a stream ==\n";
    {
        std::istringstream is("alpha\nbeta\ngamma");
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(is, line))
            lines.push_back(line);
        ICHECK(lines.size() == 3);
        ICHECK(lines[0] == "alpha" && lines[2] == "gamma");
    }

    std::cout << "== explicit classic \"C\" locale (US/static) ==\n";
    {
        // The only locale we support is the classic "C" locale. Imbuing it must
        // succeed, and its numpunct decimal point is '.'.
        std::locale c = std::locale::classic();
        const auto &np = std::use_facet<std::numpunct<char>>(c);
        ICHECK(np.decimal_point() == '.');

        std::ostringstream os;
        os.imbue(c);
        os << 1234.5;
        ICHECK(os.str() == "1234.5");
    }

    std::cout << "\n-------------------------------------\n";
    std::cout << "iostream checks run: " << g_checks
              << ", failures: " << g_fails << '\n';
    if (g_fails == 0) {
        std::cout << "RESULT: ALL IOSTREAM TESTS PASSED" << std::endl;
        return 0;
    }
    std::cout << "RESULT: " << g_fails << " IOSTREAM TEST(S) FAILED" << std::endl;
    return 1;
}
