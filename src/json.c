#include <string.h>
#include <stdint.h>

#include "json.h"
#include "utf8.h"

static void skip_ws(JsonScan *s)
{
    while (s->p < s->end &&
           (*s->p == ' ' || *s->p == '\t' || *s->p == '\n' || *s->p == '\r'))
        s->p++;
}

static bool at(JsonScan *s, char c)
{
    skip_ws(s);
    return s->p < s->end && *s->p == c;
}

static bool eat(JsonScan *s, char c)
{
    if (!at(s, c)) return false;
    s->p++;
    return true;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Читает строку в кавычках. Если out == NULL, строка просто пропускается.
static bool read_string(JsonScan *s, char *out, size_t cap)
{
    if (!eat(s, '"')) return false;

    size_t n = 0;
    while (s->p < s->end && *s->p != '"') {
        char c = *s->p++;

        if (c != '\\') {
            if (out && n + 1 < cap) out[n++] = c;
            continue;
        }

        if (s->p >= s->end) return false;
        char esc = *s->p++;
        char decoded = 0;
        switch (esc) {
        case 'n': decoded = '\n'; break;
        case 't': decoded = '\t'; break;
        case 'r': decoded = '\r'; break;
        case 'b': decoded = '\b'; break;
        case 'f': decoded = '\f'; break;
        case '"': case '\\': case '/': decoded = esc; break;
        case 'u': {
            if (s->end - s->p < 4) return false;
            uint32_t cp = 0;
            for (int i = 0; i < 4; i++) {
                int d = hex_digit(s->p[i]);
                if (d < 0) return false;
                cp = cp * 16 + (uint32_t)d;
            }
            s->p += 4;

            // Суррогатная пара: символы вне BMP приходят двумя \u подряд.
            if (cp >= 0xD800 && cp <= 0xDBFF && s->end - s->p >= 6 &&
                s->p[0] == '\\' && s->p[1] == 'u') {
                uint32_t lo = 0;
                bool ok = true;
                for (int i = 0; i < 4; i++) {
                    int d = hex_digit(s->p[2 + i]);
                    if (d < 0) { ok = false; break; }
                    lo = lo * 16 + (uint32_t)d;
                }
                if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    s->p += 6;
                }
            }

            char u8[4];
            int len = utf8_encode(cp, u8);
            for (int i = 0; i < len; i++)
                if (out && n + 1 < cap) out[n++] = u8[i];
            continue;
        }
        default:
            return false;
        }
        if (out && n + 1 < cap) out[n++] = decoded;
    }

    if (s->p >= s->end) return false;
    s->p++;   // закрывающая кавычка
    if (out && cap > 0) out[n] = '\0';
    return true;
}

// Пропускает значение любого вида, включая вложенные структуры.
static bool skip_value(JsonScan *s)
{
    skip_ws(s);
    if (s->p >= s->end) return false;

    if (*s->p == '"') return read_string(s, NULL, 0);

    if (*s->p == '{' || *s->p == '[') {
        char open = *s->p;
        char close = (open == '{') ? '}' : ']';
        int depth = 0;
        while (s->p < s->end) {
            if (*s->p == '"') {
                if (!read_string(s, NULL, 0)) return false;
                continue;
            }
            if (*s->p == open) depth++;
            else if (*s->p == close && --depth == 0) { s->p++; return true; }
            s->p++;
        }
        return false;
    }

    // Число, true, false, null — до ближайшего разделителя.
    while (s->p < s->end && *s->p != ',' && *s->p != '}' && *s->p != ']')
        s->p++;
    return true;
}

bool json_scan_init(JsonScan *s, const char *text, size_t len)
{
    s->p = text;
    s->end = text + len;
    return eat(s, '[');
}

bool json_scan_object(JsonScan *s)
{
    skip_ws(s);
    // Между элементами массива стоит запятая; перед первым её нет.
    eat(s, ',');
    return eat(s, '{');
}

bool json_scan_field(JsonScan *s, char *key, size_t key_cap,
                     char *val, size_t val_cap)
{
    skip_ws(s);
    if (eat(s, '}')) return false;
    eat(s, ',');

    if (!read_string(s, key, key_cap)) return false;
    if (!eat(s, ':')) return false;

    skip_ws(s);
    if (s->p < s->end && *s->p == '"')
        return read_string(s, val, val_cap);

    // Значение не строка — отдаём пустую строку, но поле не теряем.
    if (val && val_cap > 0) val[0] = '\0';
    return skip_value(s);
}
