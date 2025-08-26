// File: daemon/jsmn.c
#include "jsmn.h"

static int jsmn_eq(const char *a, size_t a_len, const char *b) {
  size_t i = 0;
  for (i = 0; i < a_len && b[i] != '\0'; i++) {
    if (a[i] != b[i]) return 0;
  }
  return (i == a_len) && (b[i] == '\0');
}

static jsmntok_t *jsmn_alloc_token(jsmn_parser *parser, jsmntok_t *tokens,
                                   size_t num_tokens) {
  if (parser->toknext >= num_tokens) return NULL;
  jsmntok_t *tok = &tokens[parser->toknext++];
  tok->start = tok->end = -1;
  tok->size = 0;
#ifdef JSMN_PARENT_LINKS
  tok->parent = -1;
#endif
  return tok;
}

static void jsmn_fill_token(jsmntok_t *token, jsmntype_t type, int start, int end) {
  token->type = type;
  token->start = start;
  token->end = end;
  token->size = 0;
}

static int jsmn_parse_primitive(jsmn_parser *parser, const char *js,
                                size_t len, jsmntok_t *tokens, size_t num_tokens) {
  int start = (int)parser->pos;
  for (; parser->pos < len; parser->pos++) {
    char c = js[parser->pos];
    switch (c) {
      case '\t': case '\r': case '\n': case ' ': case ',': case ']': case '}':
        goto found;
      default:
        if (c < 32 || c == 127) {
          parser->pos = (unsigned int)start;
          return JSMN_ERROR_INVAL;
        }
    }
  }
found:
  jsmntok_t *token = jsmn_alloc_token(parser, tokens, num_tokens);
  if (token == NULL) {
    parser->pos = (unsigned int)start;
    return JSMN_ERROR_NOMEM;
  }
  jsmn_fill_token(token, JSMN_PRIMITIVE, start, (int)parser->pos);
#ifdef JSMN_PARENT_LINKS
  token->parent = parser->toksuper;
#endif
  parser->pos--;
  return 0;
}

static int jsmn_parse_string(jsmn_parser *parser, const char *js,
                             size_t len, jsmntok_t *tokens, size_t num_tokens) {
  int start = (int)parser->pos;
  parser->pos++;
  for (; parser->pos < len; parser->pos++) {
    char c = js[parser->pos];
    if (c == '\"') {
      jsmntok_t *token = jsmn_alloc_token(parser, tokens, num_tokens);
      if (token == NULL) {
        parser->pos = (unsigned int)start;
        return JSMN_ERROR_NOMEM;
      }
      jsmn_fill_token(token, JSMN_STRING, start + 1, (int)parser->pos);
#ifdef JSMN_PARENT_LINKS
      token->parent = parser->toksuper;
#endif
      return 0;
    }
    if (c == '\\' && parser->pos + 1 < len) {
      parser->pos++;
      switch (js[parser->pos]) {
        case '\"': case '/': case '\\': case 'b':
        case 'f': case 'r': case 'n': case 't':
          break;
        case 'u':
          parser->pos += 4;
          break;
        default:
          parser->pos = (unsigned int)start;
          return JSMN_ERROR_INVAL;
      }
    }
  }
  parser->pos = (unsigned int)start;
  return JSMN_ERROR_PART;
}

void jsmn_init(jsmn_parser *parser) {
  parser->pos = 0U;
  parser->toknext = 0U;
  parser->toksuper = -1;
}

int jsmn_parse(jsmn_parser *parser, const char *js, size_t len,
               jsmntok_t *tokens, unsigned int num_tokens) {
  int r;
  int i;
  jsmntok_t *token;

  for (; parser->pos < len; parser->pos++) {
    char c;
    jsmntype_t type;

    c = js[parser->pos];
    switch (c) {
      case '{': case '[':
        token = jsmn_alloc_token(parser, tokens, num_tokens);
        if (token == NULL) return JSMN_ERROR_NOMEM;
        if (c == '{') type = JSMN_OBJECT; else type = JSMN_ARRAY;
        jsmn_fill_token(token, type, (int)parser->pos, -1);
#ifdef JSMN_PARENT_LINKS
        token->parent = parser->toksuper;
#endif
        parser->toksuper = (int)(parser->toknext - 1U);
        break;
      case '}': case ']':
        type = (c == '}') ? JSMN_OBJECT : JSMN_ARRAY;
        for (i = (int)(parser->toknext - 1U); i >= 0; i--) {
          token = &tokens[i];
          if (token->start != -1 && token->end == -1) {
            if (token->type != type) return JSMN_ERROR_INVAL;
            token->end = (int)(parser->pos + 1U);
            parser->toksuper = -1;
            for (i = (int)(parser->toknext - 1U); i >= 0; i--) {
              if (tokens[i].start != -1 && tokens[i].end == -1) {
                parser->toksuper = i;
                break;
              }
            }
            break;
          }
        }
        break;
      case '\"':
        r = jsmn_parse_string(parser, js, len, tokens, num_tokens);
        if (r < 0) return r;
        if (parser->toksuper != -1) tokens[parser->toksuper].size++;
        break;
      case '\t': case '\r': case '\n': case ' ':
      case ':': case ',':
        break;
      default:
        r = jsmn_parse_primitive(parser, js, len, tokens, num_tokens);
        if (r < 0) return r;
        if (parser->toksuper != -1) tokens[parser->toksuper].size++;
        break;
    }
  }

  for (i = (int)(parser->toknext - 1U); i >= 0; i--) {
    if (tokens[i].start != -1 && tokens[i].end == -1) return JSMN_ERROR_PART;
  }
  return (int)parser->toknext;
}
