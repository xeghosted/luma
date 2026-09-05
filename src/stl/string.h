#pragma once
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#ifndef STL_STRING_CAP
#define STL_STRING_CAP 96
#endif

namespace stl {
    class string {
    public:
        string() { m_buf[0] = 0; }
        string(const char* s) { set(s); }

        void set(const char* s) {
            if (!s) { m_buf[0] = 0; return; }
            size_t n = strlen(s);
            if (n > STL_STRING_CAP - 1) n = STL_STRING_CAP - 1;
            memcpy(m_buf, s, n);
            m_buf[n] = 0;
        }

        const char* c_str() const { return m_buf; }
        size_t length() const { return strlen(m_buf); }
        bool empty() const { return m_buf[0] == 0; }
        int compare(const char* s) const { return strcmp(m_buf, s ? s : ""); }

        bool operator==(const char* s) const { return compare(s) == 0; }

        string operator+(const char* s) const {
            string out(m_buf);
            size_t cur = out.length();
            if (s && cur < STL_STRING_CAP - 1) {
                size_t room = STL_STRING_CAP - 1 - cur;
                size_t n = strlen(s);
                if (n > room) n = room;
                memcpy(out.m_buf + cur, s, n);
                out.m_buf[cur + n] = 0;
            }
            return out;
        }
        string operator+(const string& o) const { return (*this) + o.c_str(); }

        static string format(const char* fmt, ...) {
            string out;
            va_list ap; va_start(ap, fmt);
            vsnprintf(out.m_buf, STL_STRING_CAP, fmt, ap);
            va_end(ap);
            return out;
        }

    private:
        char m_buf[STL_STRING_CAP];
    };
}
