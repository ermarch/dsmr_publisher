/*
 * Public Domain (www.unlicense.org)
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or distribute
 * this software, either in source code form or as a compiled binary, for any
 * purpose, commercial or non-commercial, and by any means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors of
 * this software dedicate any and all copyright interest in the software to
 * the public domain. We make this dedication for the benefit of the public at
 * large and to the detriment of our heirs and successors. We intend this
 * dedication to be an overt act of relinquishment in perpetuity of all
 * present and future rights to this software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <stdbool.h>

#define MAX_LINE 1024
#define MAX_SECTION 128
#define MAX_PARAM 128
#define MAX_VALUE 256

typedef struct ConfigParam {
    char key[MAX_PARAM];
    char value[MAX_VALUE];
    struct ConfigParam *next;
} ConfigParam;

typedef struct ConfigSection {
    char name[MAX_SECTION];
    ConfigParam *params;
    struct ConfigSection *next;
} ConfigSection;

typedef struct Config {
    ConfigSection *sections;
} Config;

Config* load_config(const char*);
const char* get_config_string(Config*, const char*, const char*, const char*);
int get_config_int(Config*, const char*, const char*, int);
float get_config_float(Config*g, const char*, const char*, float);
bool get_config_bool(Config*, const char*, const char*, bool);
void free_config(Config*);
