/* json.c - JSON encoder and decoder for Spark. */
#include "json.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buf.h"
#include "memory.h"
#include "object.h"

/* ------------------------------------------------------------------ */
/* JSON Encoder                                                       */
/* ------------------------------------------------------------------ */

static bool encode_value(Value val, buf_t *out, char *error, size_t error_size, int depth)
{
    if (depth > 512) {
        snprintf(error, error_size, "nesting depth exceeded 512");
        return false;
    }

    switch (val.type) {
    case VAL_NIL:
        buf_append(out, "null", 4);
        return true;

    case VAL_BOOL:
        if (AS_BOOL(val)) {
            buf_append(out, "true", 4);
        } else {
            buf_append(out, "false", 5);
        }
        return true;

    case VAL_INT:
        buf_appendf(out, "%lld", (long long)AS_INT(val));
        return true;

    case VAL_NUMBER: {
        double d = AS_NUMBER(val);
        if (isnan(d) || isinf(d)) {
            snprintf(error, error_size, "cannot serialize NaN or Infinity to JSON");
            return false;
        }
        char temp[64];
        snprintf(temp, sizeof(temp), "%.16g", d);
        buf_append_str(out, temp);
        return true;
    }

    case VAL_OBJ: {
        Obj *obj = AS_OBJ(val);
        switch (obj->type) {
        case OBJ_STRING: {
            ObjString *str = (ObjString *)obj;
            buf_append(out, "\"", 1);
            for (int i = 0; i < str->length; i++) {
                char c = str->chars[i];
                switch (c) {
                case '"':  buf_append(out, "\\\"", 2); break;
                case '\\': buf_append(out, "\\\\", 2); break;
                case '\b': buf_append(out, "\\b", 2);  break;
                case '\f': buf_append(out, "\\f", 2);  break;
                case '\n': buf_append(out, "\\n", 2);  break;
                case '\r': buf_append(out, "\\r", 2);  break;
                case '\t': buf_append(out, "\\t", 2);  break;
                default:
                    if ((unsigned char)c < 0x20) {
                        buf_appendf(out, "\\u%04x", (unsigned char)c);
                    } else {
                        buf_append(out, &c, 1);
                    }
                    break;
                }
            }
            buf_append(out, "\"", 1);
            return true;
        }

        case OBJ_ARRAY: {
            ObjArray *arr = (ObjArray *)obj;
            buf_append(out, "[", 1);
            for (int i = 0; i < arr->count; i++) {
                if (i > 0) {
                    buf_append(out, ", ", 2);
                }
                if (!encode_value(arr->items[i], out, error, error_size, depth + 1)) {
                    return false;
                }
            }
            buf_append(out, "]", 1);
            return true;
        }

        case OBJ_MAP: {
            ObjMap *map = (ObjMap *)obj;
            buf_append(out, "{", 1);
            bool first = true;
            for (int i = 0; i < map->capacity; i++) {
                if (map->entries[i].state == 1) {
                    if (!first) {
                        buf_append(out, ", ", 2);
                    }
                    first = false;

                    Value k = map->entries[i].key;
                    if (is_obj_type(k, OBJ_STRING)) {
                        if (!encode_value(k, out, error, error_size, depth + 1)) {
                            return false;
                        }
                    } else {
                        buf_append(out, "\"", 1);
                        ObjString *s = value_to_string(k);
                        buf_append(out, s->chars, (size_t)s->length);
                        buf_append(out, "\"", 1);
                    }
                    buf_append(out, ": ", 2);
                    if (!encode_value(map->entries[i].value, out, error, error_size, depth + 1)) {
                        return false;
                    }
                }
            }
            buf_append(out, "}", 1);
            return true;
        }

        default:
            snprintf(error, error_size, "cannot serialize %s to JSON", obj_type_name(obj->type));
            return false;
        }
    }
    }

    snprintf(error, error_size, "unknown value type");
    return false;
}

ObjString *json_encode(Value value, char *error, size_t error_size)
{
    buf_t out;
    buf_init(&out);

    mem_pause_gc();
    if (!encode_value(value, &out, error, error_size, 0)) {
        mem_resume_gc();
        buf_free(&out);
        return NULL;
    }

    ObjString *result = object_string(out.data ? out.data : "", (int)out.len);
    mem_resume_gc();
    buf_free(&out);
    return result;
}

/* ------------------------------------------------------------------ */
/* JSON Decoder                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *source;
    int length;
    int index;
    int line;
    int column;
    char *error;
    size_t error_size;
} JsonParser;

static void json_error(JsonParser *p, const char *msg)
{
    if (p->error && p->error_size > 0 && p->error[0] == '\0') {
        snprintf(p->error, p->error_size, "JSON parse error at line %d, column %d: %s",
                 p->line, p->column, msg);
    }
}

static bool json_has_error(JsonParser *p)
{
    return p->error && p->error[0] != '\0';
}

static char json_peek(JsonParser *p)
{
    if (p->index >= p->length) {
        return '\0';
    }
    return p->source[p->index];
}

static char json_advance(JsonParser *p)
{
    if (p->index >= p->length) {
        return '\0';
    }
    char c = p->source[p->index++];
    if (c == '\n') {
        p->line++;
        p->column = 1;
    } else {
        p->column++;
    }
    return c;
}

static void json_skip_whitespace(JsonParser *p)
{
    while (p->index < p->length) {
        char c = json_peek(p);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            json_advance(p);
        } else {
            break;
        }
    }
}

static bool json_match(JsonParser *p, const char *expected)
{
    int len = (int)strlen(expected);
    if (p->index + len > p->length) {
        return false;
    }
    if (memcmp(p->source + p->index, expected, (size_t)len) == 0) {
        for (int i = 0; i < len; i++) {
            json_advance(p);
        }
        return true;
    }
    return false;
}

/* Forward declaration */
static Value parse_value(JsonParser *p);

static Value parse_string_value(JsonParser *p)
{
    if (json_advance(p) != '"') {
        json_error(p, "expected '\"'");
        return NIL_VAL;
    }

    buf_t buf;
    buf_init(&buf);

    while (p->index < p->length) {
        char c = json_advance(p);
        if (c == '"') {
            ObjString *str = object_string(buf.data ? buf.data : "", (int)buf.len);
            buf_free(&buf);
            return OBJ_VAL(str);
        }
        if (c == '\\') {
            if (p->index >= p->length) {
                buf_free(&buf);
                json_error(p, "unfinished escape sequence");
                return NIL_VAL;
            }
            char esc = json_advance(p);
            switch (esc) {
            case '"':  buf_append(&buf, "\"", 1); break;
            case '\\': buf_append(&buf, "\\", 1); break;
            case '/':  buf_append(&buf, "/", 1);  break;
            case 'b':  buf_append(&buf, "\b", 1); break;
            case 'f':  buf_append(&buf, "\f", 1); break;
            case 'n':  buf_append(&buf, "\n", 1); break;
            case 'r':  buf_append(&buf, "\r", 1); break;
            case 't':  buf_append(&buf, "\t", 1); break;
            case 'u': {
                uint32_t code = 0;
                for (int i = 0; i < 4; i++) {
                    char h = json_advance(p);
                    code <<= 4;
                    if (h >= '0' && h <= '9') code |= (uint32_t)(h - '0');
                    else if (h >= 'a' && h <= 'f') code |= (uint32_t)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') code |= (uint32_t)(h - 'A' + 10);
                    else {
                        buf_free(&buf);
                        json_error(p, "invalid unicode escape in string");
                        return NIL_VAL;
                    }
                }
                /* Handle UTF-16 surrogate pairs */
                if (code >= 0xd800 && code <= 0xdbff) {
                    if (p->index + 2 <= p->length && p->source[p->index] == '\\' && p->source[p->index + 1] == 'u') {
                        json_advance(p); /* '\\' */
                        json_advance(p); /* 'u' */
                        uint32_t low = 0;
                        for (int i = 0; i < 4; i++) {
                            char h = json_advance(p);
                            low <<= 4;
                            if (h >= '0' && h <= '9') low |= (uint32_t)(h - '0');
                            else if (h >= 'a' && h <= 'f') low |= (uint32_t)(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') low |= (uint32_t)(h - 'A' + 10);
                            else {
                                buf_free(&buf);
                                json_error(p, "invalid unicode surrogate escape in string");
                                return NIL_VAL;
                            }
                        }
                        if (low >= 0xdc00 && low <= 0xdfff) {
                            code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
                        }
                    }
                }
                /* Encode UTF-8 */
                if (code <= 0x7f) {
                    char ch = (char)code;
                    buf_append(&buf, &ch, 1);
                } else if (code <= 0x7ff) {
                    char chs[2] = {
                        (char)(0xc0 | (code >> 6)),
                        (char)(0x80 | (code & 0x3f))
                    };
                    buf_append(&buf, chs, 2);
                } else if (code <= 0xffff) {
                    char chs[3] = {
                        (char)(0xe0 | (code >> 12)),
                        (char)(0x80 | ((code >> 6) & 0x3f)),
                        (char)(0x80 | (code & 0x3f))
                    };
                    buf_append(&buf, chs, 3);
                } else {
                    char chs[4] = {
                        (char)(0xf0 | (code >> 18)),
                        (char)(0x80 | ((code >> 12) & 0x3f)),
                        (char)(0x80 | ((code >> 6) & 0x3f)),
                        (char)(0x80 | (code & 0x3f))
                    };
                    buf_append(&buf, chs, 4);
                }
                break;
            }
            default:
                buf_free(&buf);
                json_error(p, "invalid escape sequence");
                return NIL_VAL;
            }
        } else if ((unsigned char)c < 0x20) {
            buf_free(&buf);
            json_error(p, "unescaped control character in string");
            return NIL_VAL;
        } else {
            buf_append(&buf, &c, 1);
        }
    }

    buf_free(&buf);
    json_error(p, "unterminated string");
    return NIL_VAL;
}

static Value parse_number_value(JsonParser *p)
{
    int start = p->index;
    bool is_float = false;

    if (json_peek(p) == '-') {
        json_advance(p);
    }

    if (p->index >= p->length || !isdigit((unsigned char)json_peek(p))) {
        json_error(p, "expected digits in number");
        return NIL_VAL;
    }

    char first_digit = json_advance(p);
    if (first_digit == '0') {
        if (isdigit((unsigned char)json_peek(p))) {
            json_error(p, "leading zeros are not permitted in JSON numbers");
            return NIL_VAL;
        }
    } else {
        while (isdigit((unsigned char)json_peek(p))) {
            json_advance(p);
        }
    }

    if (json_peek(p) == '.') {
        is_float = true;
        json_advance(p);
        if (!isdigit((unsigned char)json_peek(p))) {
            json_error(p, "expected digits after decimal point");
            return NIL_VAL;
        }
        while (isdigit((unsigned char)json_peek(p))) {
            json_advance(p);
        }
    }

    if (json_peek(p) == 'e' || json_peek(p) == 'E') {
        is_float = true;
        json_advance(p);
        if (json_peek(p) == '+' || json_peek(p) == '-') {
            json_advance(p);
        }
        if (!isdigit((unsigned char)json_peek(p))) {
            json_error(p, "expected digits in exponent");
            return NIL_VAL;
        }
        while (isdigit((unsigned char)json_peek(p))) {
            json_advance(p);
        }
    }

    int len = p->index - start;
    char num_buf[128];
    if (len >= (int)sizeof(num_buf)) {
        json_error(p, "number too long");
        return NIL_VAL;
    }
    memcpy(num_buf, p->source + start, (size_t)len);
    num_buf[len] = '\0';

    if (!is_float) {
        char *endptr = NULL;
        errno = 0;
        long long val = strtoll(num_buf, &endptr, 10);
        if (errno == 0 && endptr == num_buf + len) {
            return INT_VAL((int64_t)val);
        }
    }

    char *endptr = NULL;
    double dval = strtod(num_buf, &endptr);
    return NUMBER_VAL(dval);
}

static Value parse_array_value(JsonParser *p)
{
    if (json_advance(p) != '[') {
        json_error(p, "expected '['");
        return NIL_VAL;
    }

    ObjArray *arr = object_array();

    json_skip_whitespace(p);
    if (json_peek(p) == ']') {
        json_advance(p);
        return OBJ_VAL(arr);
    }

    for (;;) {
        json_skip_whitespace(p);
        Value item = parse_value(p);
        if (json_has_error(p)) {
            return NIL_VAL;
        }
        array_push(arr, item);

        json_skip_whitespace(p);
        char c = json_peek(p);
        if (c == ']') {
            json_advance(p);
            return OBJ_VAL(arr);
        }
        if (c == ',') {
            json_advance(p);
            continue;
        }
        json_error(p, "expected ',' or ']' in array");
        return NIL_VAL;
    }
}

static Value parse_object_value(JsonParser *p)
{
    if (json_advance(p) != '{') {
        json_error(p, "expected '{'");
        return NIL_VAL;
    }

    ObjMap *map = object_map();

    json_skip_whitespace(p);
    if (json_peek(p) == '}') {
        json_advance(p);
        return OBJ_VAL(map);
    }

    for (;;) {
        json_skip_whitespace(p);
        if (json_peek(p) != '"') {
            json_error(p, "expected string key in object");
            return NIL_VAL;
        }

        Value key = parse_string_value(p);
        if (json_has_error(p)) {
            return NIL_VAL;
        }

        json_skip_whitespace(p);
        if (json_peek(p) != ':') {
            json_error(p, "expected ':' after key in object");
            return NIL_VAL;
        }
        json_advance(p);

        json_skip_whitespace(p);
        Value val = parse_value(p);
        if (json_has_error(p)) {
            return NIL_VAL;
        }

        map_set(map, key, val);

        json_skip_whitespace(p);
        char c = json_peek(p);
        if (c == '}') {
            json_advance(p);
            return OBJ_VAL(map);
        }
        if (c == ',') {
            json_advance(p);
            continue;
        }
        json_error(p, "expected ',' or '}' in object");
        return NIL_VAL;
    }
}

static Value parse_value(JsonParser *p)
{
    json_skip_whitespace(p);
    char c = json_peek(p);

    if (c == 'n') {
        if (json_match(p, "null")) {
            return NIL_VAL;
        }
        json_error(p, "unexpected token");
        return NIL_VAL;
    }
    if (c == 't') {
        if (json_match(p, "true")) {
            return BOOL_VAL(true);
        }
        json_error(p, "unexpected token");
        return NIL_VAL;
    }
    if (c == 'f') {
        if (json_match(p, "false")) {
            return BOOL_VAL(false);
        }
        json_error(p, "unexpected token");
        return NIL_VAL;
    }
    if (c == '"') {
        return parse_string_value(p);
    }
    if (c == '[') {
        return parse_array_value(p);
    }
    if (c == '{') {
        return parse_object_value(p);
    }
    if (c == '-' || isdigit((unsigned char)c)) {
        return parse_number_value(p);
    }

    if (c == '\0') {
        json_error(p, "unexpected end of JSON input");
    } else {
        json_error(p, "unexpected character");
    }
    return NIL_VAL;
}

Value json_decode(const char *text, int length, char *error, size_t error_size)
{
    JsonParser parser;
    parser.source = text;
    parser.length = length;
    parser.index = 0;
    parser.line = 1;
    parser.column = 1;
    parser.error = error;
    parser.error_size = error_size;
    if (error && error_size > 0) {
        error[0] = '\0';
    }

    mem_pause_gc();
    Value val = parse_value(&parser);
    if (json_has_error(&parser)) {
        mem_resume_gc();
        return NIL_VAL;
    }

    json_skip_whitespace(&parser);
    if (parser.index < parser.length) {
        json_error(&parser, "trailing characters after JSON value");
        mem_resume_gc();
        return NIL_VAL;
    }

    mem_resume_gc();
    return val;
}
