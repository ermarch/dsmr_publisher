/*
* Features of this upgraded parser:
* + Default values - Returns a fallback if a key is missing.
* + Type-safe lookups - Supports string, int, float, bool.
* + Case-insensitive keys and sections - Matches Hostname and hostname equally.
* + Memory-efficient - Reads the file once into linked structures.
* + Comment handling - Lines starting with # or ; ignored.
* + Sectioned parsing - Only returns parameters from the correct [section].
* + Boolean support - Recognizes "true", "yes", "1", "on" as true.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

#include "config.h"

// Utility: trim leading/trailing whitespace
static void trim(char *str)
{
    char *start = str;
    char *end;

    while (isspace((unsigned char)*start)) start++;

    if (start != str)
        memmove(str, start, strlen(start) + 1);

    end = str + strlen(str) - 1;
    while (end >= str && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }
}

// Remove inline comments while respecting quoted strings
static void strip_inline_comment(char *str)
{
    int in_quote = 0;

    for (char *p = str; *p; p++) {
        if (*p == '"' || *p == '\'')
            in_quote = !in_quote;

        if (!in_quote && (*p == '#' || *p == ';')) {
            *p = '\0';
            break;
        }
    }
}

// Find first separator (: or =) that is not inside quotes
static char* find_separator(char *str)
{
    int in_quote = 0;

    for (char *p = str; *p; p++) {
        if (*p == '"' || *p == '\'')
            in_quote = !in_quote;

        if (!in_quote && (*p == ':' || *p == '='))
            return p;
    }
    return NULL;
}

// Case-insensitive string compare
static int stricmp(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == *b;
}

ConfigParam* create_param(const char *key, const char *value) {
    ConfigParam *p = malloc(sizeof(ConfigParam));
    strncpy(p->key, key, MAX_PARAM-1);
    p->key[MAX_PARAM-1] = '\0';
    strncpy(p->value, value, MAX_VALUE-1);
    p->value[MAX_VALUE-1] = '\0';
    p->next = NULL;
    return p;
}

ConfigSection* create_section(const char *name) {
    ConfigSection *s = malloc(sizeof(ConfigSection));
    strncpy(s->name, name, MAX_SECTION-1);
    s->name[MAX_SECTION-1] = '\0';
    s->params = NULL;
    s->next = NULL;
    return s;
}

Config* load_config(const char *filename) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        perror("Failed to open config file");
        return NULL;
    }

    Config *cfg = malloc(sizeof(Config));
    cfg->sections = NULL;
    ConfigSection *current_section = NULL;

    char line[MAX_LINE];
    while (fgets(line, sizeof(line), file)) {
        trim(line);
        if (line[0] == '\0' || line[0] == '#' || line[0] == ';') continue;

        if (line[0] == '[') {
            char *end_bracket = strchr(line, ']');
            if (!end_bracket) continue;
            *end_bracket = '\0';
            ConfigSection *section = create_section(line + 1);
            section->next = cfg->sections;
            cfg->sections = section;
            current_section = section;
            continue;
        }

//      char *colon = strchr(line, ':');
//      if (colon && current_section) {
        strip_inline_comment(line);
        trim(line);

        char *sep = find_separator(line);

        if (sep && current_section) {
            *sep = '\0';

            char *key = line;
            char *val = sep + 1;

            trim(key);
            trim(val);

            // Remove optional surrounding quotes from value
            if ((*val == '"' && val[strlen(val)-1] == '"') ||
                (*val == '\'' && val[strlen(val)-1] == '\'')) {
                val[strlen(val)-1] = '\0';
                val++;
            }

            ConfigParam *param = create_param(key, val);
            param->next = current_section->params;
            current_section->params = param;
        }
    }

    fclose(file);
    return cfg;
}

// Generic string lookup (case-insensitive)
const char* get_config_string(Config *cfg, const char *section, const char *param, const char *default_val) {
    for (ConfigSection *s = cfg->sections; s; s = s->next) {
        if (stricmp(s->name, section)) {
            for (ConfigParam *p = s->params; p; p = p->next) {
                if (stricmp(p->key, param)) return p->value;
            }
        }
    }
    return default_val;
}

// Lookup as int
int get_config_int(Config *cfg, const char *section, const char *param, int default_val) {
    const char *val = get_config_string(cfg, section, param, NULL);
    if (!val) return default_val;
    return atoi(val);
}

// Lookup as float
float get_config_float(Config *cfg, const char *section, const char *param, float default_val) {
    const char *val = get_config_string(cfg, section, param, NULL);
    if (!val) return default_val;
    return strtof(val, NULL);
}

// Lookup as bool (true = "1", "yes", "true", "on"; false = "0", "no", "false", "off")
bool get_config_bool(Config *cfg, const char *section, const char *param, bool default_val) {
    const char *val = get_config_string(cfg, section, param, NULL);
    if (!val) return default_val;

    if (stricmp(val, "1") || stricmp(val, "yes") || stricmp(val, "true") || stricmp(val, "on")) return true;
    if (stricmp(val, "0") || stricmp(val, "no") || stricmp(val, "false") || stricmp(val, "off")) return false;
    return default_val;
}

void free_config(Config *cfg) {
    while (cfg->sections) {
        ConfigSection *s = cfg->sections;
        cfg->sections = s->next;
        while (s->params) {
            ConfigParam *p = s->params;
            s->params = p->next;
            free(p);
        }
        free(s);
    }
    free(cfg);
}
