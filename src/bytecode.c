#include "bytecode.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "memory.h"
#include "value.h"
#include "vm.h"

enum {
    CONST_TAG_NIL = 0,
    CONST_TAG_BOOL = 1,
    CONST_TAG_INT = 2,
    CONST_TAG_NUMBER = 3,
    CONST_TAG_STRING = 4,
    CONST_TAG_FUNCTION = 5
};

static bool write_u8(FILE *f, uint8_t val)
{
    return fputc(val, f) != EOF;
}

static bool write_u32(FILE *f, uint32_t val)
{
    uint8_t bytes[4] = {
        (uint8_t)((val >> 24) & 0xff),
        (uint8_t)((val >> 16) & 0xff),
        (uint8_t)((val >> 8) & 0xff),
        (uint8_t)(val & 0xff)
    };
    return fwrite(bytes, 1, 4, f) == 4;
}

static bool write_u64(FILE *f, uint64_t val)
{
    uint8_t bytes[8] = {
        (uint8_t)((val >> 56) & 0xff),
        (uint8_t)((val >> 48) & 0xff),
        (uint8_t)((val >> 40) & 0xff),
        (uint8_t)((val >> 32) & 0xff),
        (uint8_t)((val >> 24) & 0xff),
        (uint8_t)((val >> 16) & 0xff),
        (uint8_t)((val >> 8) & 0xff),
        (uint8_t)(val & 0xff)
    };
    return fwrite(bytes, 1, 8, f) == 8;
}

static bool write_double(FILE *f, double val)
{
    uint64_t bits;
    memcpy(&bits, &val, sizeof(bits));
    return write_u64(f, bits);
}

/* Zigzag-encoded LEB128 varint. Line and column deltas are usually zero or
 * small, so this uses one byte where the old encoding used four. */
static bool write_varint(FILE *f, int64_t value)
{
    uint64_t bits = (uint64_t)value;
    uint64_t zigzag = (bits << 1) ^ (value < 0 ? UINT64_MAX : 0);

    while (zigzag >= 0x80) {
        if (!write_u8(f, (uint8_t)(zigzag | 0x80))) {
            return false;
        }
        zigzag >>= 7;
    }
    return write_u8(f, (uint8_t)zigzag);
}

static bool read_varint(FILE *f, int64_t *out)
{
    uint64_t result = 0;
    int shift = 0;

    for (;;) {
        int byte = fgetc(f);

        if (byte == EOF || shift > 63) {
            return false;
        }
        result |= (uint64_t)(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) {
            break;
        }
        shift += 7;
    }
    *out = (int64_t)((result >> 1) ^ (uint64_t)-((int64_t)(result & 1)));
    return true;
}

static bool write_string_data(FILE *f, const char *chars, uint32_t len)
{
    if (!write_u32(f, len)) return false;
    if (len > 0 && fwrite(chars, 1, len, f) != len) return false;
    return true;
}

static bool serialize_function(FILE *f, const ObjFunction *fn)
{
    if (!write_u32(f, (uint32_t)fn->arity)) return false;
    if (!write_u32(f, (uint32_t)fn->min_arity)) return false;
    if (!write_u8(f, fn->is_varargs ? 1 : 0)) return false;
    if (!write_u8(f, fn->is_constructor ? 1 : 0)) return false;
    if (!write_u32(f, (uint32_t)fn->upvalue_count)) return false;

    /* Name */
    if (fn->name) {
        if (!write_u8(f, 1)) return false;
        if (!write_string_data(f, fn->name->chars, (uint32_t)fn->name->length)) return false;
    } else {
        if (!write_u8(f, 0)) return false;
    }

    /* Script path */
    if (fn->script_path) {
        if (!write_u8(f, 1)) return false;
        if (!write_string_data(f, fn->script_path->chars, (uint32_t)fn->script_path->length)) return false;
    } else {
        if (!write_u8(f, 0)) return false;
    }

    /* Source text */
    if (fn->source) {
        uint32_t slen = (uint32_t)strlen(fn->source);
        if (!write_u8(f, 1)) return false;
        if (!write_string_data(f, fn->source, slen)) return false;
    } else {
        if (!write_u8(f, 0)) return false;
    }

    /* Chunk bytecode */
    uint32_t code_count = (uint32_t)fn->chunk.count;
    if (!write_u32(f, code_count)) return false;
    if (code_count > 0) {
        if (fwrite(fn->chunk.code, 1, code_count, f) != code_count) return false;
        int64_t previous_line = 0;
        int64_t previous_column = 0;
        for (uint32_t i = 0; i < code_count; i++) {
            int64_t line = fn->chunk.lines[i];
            int64_t column = fn->chunk.columns[i];

            if (!write_varint(f, line - previous_line)) return false;
            if (!write_varint(f, column - previous_column)) return false;
            previous_line = line;
            previous_column = column;
        }
    }

    /* Chunk constants */
    uint32_t const_count = (uint32_t)fn->chunk.constants_count;
    if (!write_u32(f, const_count)) return false;
    for (uint32_t i = 0; i < const_count; i++) {
        Value val = fn->chunk.constants[i];
        if (is_nil(val)) {
            if (!write_u8(f, CONST_TAG_NIL)) return false;
        } else if (is_bool(val)) {
            if (!write_u8(f, CONST_TAG_BOOL)) return false;
            if (!write_u8(f, AS_BOOL(val) ? 1 : 0)) return false;
        } else if (is_int(val)) {
            if (!write_u8(f, CONST_TAG_INT)) return false;
            if (!write_u64(f, (uint64_t)AS_INT(val))) return false;
        } else if (is_number(val)) {
            if (!write_u8(f, CONST_TAG_NUMBER)) return false;
            if (!write_double(f, AS_NUMBER(val))) return false;
        } else if (is_obj_type(val, OBJ_STRING)) {
            ObjString *str = AS_STRING(val);
            if (!write_u8(f, CONST_TAG_STRING)) return false;
            if (!write_string_data(f, str->chars, (uint32_t)str->length)) return false;
        } else if (is_obj_type(val, OBJ_FUNCTION)) {
            if (!write_u8(f, CONST_TAG_FUNCTION)) return false;
            if (!serialize_function(f, (ObjFunction *)AS_OBJ(val))) return false;
        } else {
            return false;
        }
    }

    return true;
}

bool bytecode_save(const ObjFunction *function, const char *path, char *error, size_t error_size)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        if (error && error_size > 0) {
            snprintf(error, error_size, "could not open '%s' for writing", path);
        }
        return false;
    }

    /* Magic & Version */
    if (fwrite(SPARK_BYTECODE_MAGIC, 1, 4, f) != 4 ||
        !write_u32(f, SPARK_BYTECODE_VERSION)) {
        if (error && error_size > 0) {
            snprintf(error, error_size, "error writing bytecode header to '%s'", path);
        }
        fclose(f);
        return false;
    }

    /* Globals table */
    int g_count = vm_global_count();
    uint32_t active_globals = 0;
    for (int i = 0; i < g_count; i++) {
        if (vm_global_slot_obj_name(i)) active_globals++;
    }
    if (!write_u32(f, active_globals)) {
        if (error && error_size > 0) {
            snprintf(error, error_size, "error writing globals count to '%s'", path);
        }
        fclose(f);
        return false;
    }
    for (int i = 0; i < g_count; i++) {
        ObjString *gname = vm_global_slot_obj_name(i);
        if (gname) {
            if (!write_u32(f, (uint32_t)i) ||
                !write_string_data(f, gname->chars, (uint32_t)gname->length)) {
                if (error && error_size > 0) {
                    snprintf(error, error_size, "error writing global variable to '%s'", path);
                }
                fclose(f);
                return false;
            }
        }
    }

    if (!serialize_function(f, function)) {
        if (error && error_size > 0) {
            snprintf(error, error_size, "error serializing function to '%s'", path);
        }
        fclose(f);
        return false;
    }

    fclose(f);
    return true;
}

static bool read_u8(FILE *f, uint8_t *val)
{
    int c = fgetc(f);
    if (c == EOF) return false;
    *val = (uint8_t)c;
    return true;
}

static bool read_u32(FILE *f, uint32_t *val)
{
    uint8_t bytes[4];
    if (fread(bytes, 1, 4, f) != 4) return false;
    *val = ((uint32_t)bytes[0] << 24) |
           ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8)  |
           bytes[3];
    return true;
}

static bool read_u64(FILE *f, uint64_t *val)
{
    uint8_t bytes[8];
    if (fread(bytes, 1, 8, f) != 8) return false;
    *val = ((uint64_t)bytes[0] << 56) |
           ((uint64_t)bytes[1] << 48) |
           ((uint64_t)bytes[2] << 40) |
           ((uint64_t)bytes[3] << 32) |
           ((uint64_t)bytes[4] << 24) |
           ((uint64_t)bytes[5] << 16) |
           ((uint64_t)bytes[6] << 8)  |
           bytes[7];
    return true;
}

static bool read_double(FILE *f, double *val)
{
    uint64_t bits;
    if (!read_u64(f, &bits)) return false;
    memcpy(val, &bits, sizeof(bits));
    return true;
}

static bool read_string_data(FILE *f, char **chars_out, uint32_t *len_out)
{
    uint32_t len;
    if (!read_u32(f, &len)) return false;
    char *buf = malloc((size_t)len + 1);
    if (!buf) return false;
    if (len > 0) {
        if (fread(buf, 1, len, f) != len) {
            free(buf);
            return false;
        }
    }
    buf[len] = '\0';
    *chars_out = buf;
    *len_out = len;
    return true;
}

static ObjFunction *deserialize_function(FILE *f)
{
    uint32_t arity, min_arity, upvalue_count;
    uint8_t is_varargs, is_constructor, has_field;

    if (!read_u32(f, &arity)) return NULL;
    if (!read_u32(f, &min_arity)) return NULL;
    if (!read_u8(f, &is_varargs)) return NULL;
    if (!read_u8(f, &is_constructor)) return NULL;
    if (!read_u32(f, &upvalue_count)) return NULL;

    ObjFunction *fn = object_function();
    fn->arity = (int)arity;
    fn->min_arity = (int)min_arity;
    fn->is_varargs = is_varargs != 0;
    fn->is_constructor = is_constructor != 0;
    fn->upvalue_count = (int)upvalue_count;

    /* Name */
    if (!read_u8(f, &has_field)) return NULL;
    if (has_field) {
        char *str;
        uint32_t len;
        if (!read_string_data(f, &str, &len)) return NULL;
        fn->name = object_string(str, (int)len);
        free(str);
    }

    /* Script path */
    if (!read_u8(f, &has_field)) return NULL;
    if (has_field) {
        char *str;
        uint32_t len;
        if (!read_string_data(f, &str, &len)) return NULL;
        fn->script_path = object_string(str, (int)len);
        free(str);
    }

    /* Source text */
    if (!read_u8(f, &has_field)) return NULL;
    if (has_field) {
        char *str;
        uint32_t len;
        if (!read_string_data(f, &str, &len)) return NULL;
        fn->source = vm_add_source(str);
        free(str);
    }

    /* Chunk code */
    uint32_t code_count;
    if (!read_u32(f, &code_count)) return NULL;
    if (code_count > 0) {
        fn->chunk.count = (int)code_count;
        fn->chunk.capacity = (int)code_count;
        fn->chunk.code = malloc(code_count);
        fn->chunk.lines = malloc(code_count * sizeof(int));
        fn->chunk.columns = malloc(code_count * sizeof(int));
        if (!fn->chunk.code || !fn->chunk.lines || !fn->chunk.columns) return NULL;
        if (fread(fn->chunk.code, 1, code_count, f) != code_count) return NULL;
        int64_t line = 0;
        int64_t column = 0;
        for (uint32_t i = 0; i < code_count; i++) {
            int64_t line_delta, column_delta;

            if (!read_varint(f, &line_delta) || !read_varint(f, &column_delta)) return NULL;
            line += line_delta;
            column += column_delta;
            fn->chunk.lines[i] = (int)line;
            fn->chunk.columns[i] = (int)column;
        }
    }

    /* Chunk constants */
    uint32_t const_count;
    if (!read_u32(f, &const_count)) return NULL;
    for (uint32_t i = 0; i < const_count; i++) {
        uint8_t tag;
        if (!read_u8(f, &tag)) return NULL;
        Value val;
        switch (tag) {
        case CONST_TAG_NIL:
            val = NIL_VAL;
            break;
        case CONST_TAG_BOOL: {
            uint8_t b;
            if (!read_u8(f, &b)) return NULL;
            val = BOOL_VAL(b != 0);
            break;
        }
        case CONST_TAG_INT: {
            uint64_t num;
            if (!read_u64(f, &num)) return NULL;
            val = INT_VAL((int64_t)num);
            break;
        }
        case CONST_TAG_NUMBER: {
            double num;
            if (!read_double(f, &num)) return NULL;
            val = NUMBER_VAL(num);
            break;
        }
        case CONST_TAG_STRING: {
            char *str;
            uint32_t len;
            if (!read_string_data(f, &str, &len)) return NULL;
            val = OBJ_VAL(object_string(str, (int)len));
            free(str);
            break;
        }
        case CONST_TAG_FUNCTION: {
            ObjFunction *sub_fn = deserialize_function(f);
            if (!sub_fn) return NULL;
            val = OBJ_VAL(sub_fn);
            break;
        }
        default:
            return NULL;
        }
        chunk_add_constant(&fn->chunk, val);
    }

    return fn;
}

ObjFunction *bytecode_load(const char *path, char *error, size_t error_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (error && error_size > 0) {
            snprintf(error, error_size, "could not open '%s' for reading", path);
        }
        return NULL;
    }

    char magic[4];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, SPARK_BYTECODE_MAGIC, 4) != 0) {
        if (error && error_size > 0) {
            snprintf(error, error_size, "invalid Spark bytecode file (incorrect magic header)");
        }
        fclose(f);
        return NULL;
    }

    uint32_t version;
    if (!read_u32(f, &version)) {
        if (error && error_size > 0) {
            snprintf(error, error_size, "corrupt bytecode file: unexpected end of file reading header");
        }
        fclose(f);
        return NULL;
    }

    if (version != SPARK_BYTECODE_VERSION) {
        if (error && error_size > 0) {
            snprintf(error, error_size, "unsupported bytecode version (expected %u, got %u)",
                     (unsigned)SPARK_BYTECODE_VERSION, (unsigned)version);
        }
        fclose(f);
        return NULL;
    }

    uint32_t num_globals;
    if (!read_u32(f, &num_globals)) {
        if (error && error_size > 0) {
            snprintf(error, error_size, "corrupt bytecode file: could not read globals count");
        }
        fclose(f);
        return NULL;
    }

    mem_pause_gc();
    for (uint32_t i = 0; i < num_globals; i++) {
        uint32_t slot;
        char *str;
        uint32_t len;
        if (!read_u32(f, &slot) || !read_string_data(f, &str, &len)) {
            mem_resume_gc();
            if (error && error_size > 0) {
                snprintf(error, error_size, "corrupt bytecode file: error reading global variable");
            }
            fclose(f);
            return NULL;
        }
        vm_register_global_slot((int)slot, object_string(str, (int)len));
        free(str);
    }

    ObjFunction *fn = deserialize_function(f);
    mem_resume_gc();

    if (!fn) {
        if (error && error_size > 0) {
            snprintf(error, error_size, "corrupt bytecode file '%s'", path);
        }
        fclose(f);
        return NULL;
    }

    fclose(f);
    return fn;
}

bool bytecode_is_bytecode_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char magic[4];
    size_t n = fread(magic, 1, 4, f);
    fclose(f);
    if (n != 4) return false;
    return memcmp(magic, SPARK_BYTECODE_MAGIC, 4) == 0;
}
