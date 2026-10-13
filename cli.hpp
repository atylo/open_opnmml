// ================================================================================================================
// cli.hpp - tiny helpers shared by the command-line tools (mmlplay, mmltrace, mmldump).
//
// Every numeric option is parsed STRICTLY: the whole text must be a number inside the allowed range, otherwise an
// error naming the option and the allowed values is printed.  (Plain atoi() would turn "volume=32" into 0 and carry on.)
// ================================================================================================================
#pragma once
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace cli {

// Parse an integer (decimal, or hexadecimal with a 0x prefix) that must lie in [lo, hi].
// On failure prints "error: option <opt>: ..." to stderr and returns false.
inline bool parseInt(const char* opt, const char* text, long lo, long hi, long& out, const char* meaning, const char* example) {
    char* end = nullptr;
    errno = 0;
    long v = strtol(text, &end, 0);
    if (text[0] == '\0' || *end != '\0' || errno == ERANGE) {
        fprintf(stderr, "error: option %s: '%s' is not a whole number.\n"
                        "       Give just the number, e.g. %s %s  (%s, allowed %ld..%ld).\n",
                opt, text, opt, example, meaning, lo, hi);
        return false;
    }
    if (v < lo || v > hi) {
        fprintf(stderr, "error: option %s: %ld is out of range (%s, allowed %ld..%ld).\n", opt, v, meaning, lo, hi);
        return false;
    }
    out = v;
    return true;
}

// Parse a decimal number (may have a fraction) that must lie in [lo, hi].
inline bool parseDouble(const char* opt, const char* text, double lo, double hi, double& out, const char* meaning, const char* example) {
    char* end = nullptr;
    errno = 0;
    double v = strtod(text, &end);
    if (text[0] == '\0' || *end != '\0' || errno == ERANGE || v != v) {
        fprintf(stderr, "error: option %s: '%s' is not a number.\n"
                        "       Give just the number, e.g. %s %s  (%s, allowed %g..%g).\n", opt, text, opt, example, meaning, lo, hi);
        return false;
    }
    if (v < lo || v > hi) {
        fprintf(stderr, "error: option %s: %g is out of range (%s, allowed %g..%g).\n", opt, v, meaning, lo, hi);
        return false;
    }
    out = v;
    return true;
}

// Reads a whole file; returns false and prints "error: cannot read '<path>': <reason>" on failure.
inline bool readFile(const char* path, std::string& out) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "error: cannot open '%s' for reading: %s\n", path, strerror(errno)); return false; }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    bool ok = !ferror(f);
    fclose(f);
    if (!ok) fprintf(stderr, "error: reading '%s' failed.\n", path);
    return ok;
}

// Option matching: accepts "-t 90", "-t90", "--time 90" and "--time=90".
// Returns true if argv[i] is this option; `value` then points at its value (and i is advanced past it) or is nullptr
// when the value is missing (the caller reports that).
inline bool matchOption(int argc, char** argv, int& i, const char* shortName, const char* longName, const char*& value) {
    const char* a = argv[i];
    size_t sl = strlen(shortName), ll = strlen(longName);
    if (strncmp(a, shortName, sl) == 0 && a[sl] != '\0' && a[sl] != '=' && strncmp(a, "--", 2) != 0) { value = a + sl; return true; }   // -t90
    if (strcmp(a, shortName) == 0 || strcmp(a, longName) == 0) { value = (i + 1 < argc) ? argv[++i] : nullptr; return true; }
    if (strncmp(a, longName, ll) == 0 && a[ll] == '=') { value = a + ll + 1; return true; }
    return false;
}

}  // namespace cli
