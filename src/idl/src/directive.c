/*
 * Copyright(c) 2020 ADLINK Technology Limited and others
 *
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License v. 2.0 which is available at
 * http://www.eclipse.org/legal/epl-2.0, or the Eclipse Distribution License
 * v. 1.0 which is available at
 * http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause
 */
#include "config.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#if HAVE_XLOCALE_H
# include <xlocale.h>
#elif HAVE_LOCALE_H
# include <locale.h>
#endif
#include <string.h>

#if !HAVE_STRTOULL_L && HAVE__STRTOULL_L
#define strtoull_l(...) _strtoull_l(__VA_ARGS__)
#endif

#if !HAVE_STRTOLD_L && HAVE__STRTOLD_L
#define strtold_l(...) _strtold_l(__VA_ARGS__)
#endif

#include "idl/processor.h"
#include "idl/string.h"
#include "scope.h"

#include "parser.h"
#include "tree.h"
#include "directive.h"

static int32_t
push_line(idl_processor_t *proc, idl_line_t *dir)
{
  if (dir->file) {
    idl_file_t *file;
    for (file = proc->files; file; file = file->next) {
      if (strcmp(dir->file->value.str, file->name) == 0)
        break;
    }
    if (!file) {
      idl_file_t *last;
      if (!(file = calloc(1, sizeof(*file))))
        return IDL_RETCODE_NO_MEMORY;
      file->name = dir->file->value.str;
      if (proc->files) {
        /* maintain order to ensure the first file is actually first */
        for (last = proc->files; last->next; last = last->next) ;
        last->next = file;
      } else {
        proc->files = file;
      }
    } else {
      free(dir->file->value.str);
    }
    proc->scanner.position.file = (const char *)file->name;
    free(dir->file);
  }
  proc->scanner.position.line = (uint32_t)dir->line->value.ullng;
  proc->scanner.position.column = 1;
  free(dir->line);
  free(dir);
  proc->directive = NULL;
  return 0;
}

static int32_t
parse_line(idl_processor_t *proc, idl_token_t *tok)
{
  idl_line_t *dir = (idl_line_t *)proc->directive;
  assert(dir);

  switch (proc->state) {
    case IDL_SCAN_LINE: {
      char *end;
      unsigned long long ullng;
      idl_literal_t *lit;

      assert(dir);
      if (tok->code != IDL_TOKEN_PP_NUMBER) {
        idl_error(proc, &tok->location,
          "no line number in #line directive");
        return IDL_RETCODE_SYNTAX_ERROR;
      }
      ullng = strtoull_l(tok->value.str, &end, 10, proc->locale);
      if (end == tok->value.str || *end != '\0' || ullng > INT32_MAX) {
        idl_error(proc, &tok->location,
          "invalid line number in #line directive");
        return IDL_RETCODE_SYNTAX_ERROR;
      }
      if (!(lit = calloc(1,sizeof(*dir->line))))
        return IDL_RETCODE_NO_MEMORY;
      dir->line = lit;
      lit->node.mask = IDL_EXPR|IDL_LITERAL|IDL_ULLONG;
      lit->node.location = tok->location;
      lit->value.ullng = ullng;
      proc->state = IDL_SCAN_FILENAME;
    } break;
    case IDL_SCAN_FILENAME: {
      idl_literal_t *lit;

      assert(dir);
      proc->state = IDL_SCAN_EXTRA_TOKEN;
      if (tok->code != '\n' && tok->code != 0) {
        if (tok->code != IDL_TOKEN_STRING_LITERAL) {
          idl_error(proc, &tok->location,
            "invalid filename in #line directive");
          return IDL_RETCODE_SYNTAX_ERROR;
        }
        assert(dir && !dir->file);
        if (!(lit = calloc(1,sizeof(*lit))))
          return IDL_RETCODE_NO_MEMORY;
        dir->file = lit;
        lit->node.mask = IDL_EXPR|IDL_LITERAL|IDL_STRING;
        lit->node.location = tok->location;
        lit->value.str = tok->value.str;
        /* do not free string on return */
        tok->value.str = NULL;
        break;
      }
    } /* fall through */
    case IDL_SCAN_EXTRA_TOKEN:
      assert(dir);
      if (tok->code == '\n' || tok->code == 0) {
        proc->state = IDL_SCAN;
        return push_line(proc, dir);
      } else if (!dir->extra_tokens) {
        idl_warning(proc, &tok->location,
          "extra tokens at end of #line directive");
      }
      break;
    default:
      assert(0);
      break;
  }
  return 0;
}

static int32_t
push_keylist(idl_processor_t *proc, idl_keylist_t *dir)
{
  idl_entry_t *entry;
  idl_struct_t *_struct;
  idl_name_t *data_type;

  assert(dir);

  data_type = dir->data_type;
  if (!(entry = idl_find(proc, proc->scope, data_type))) {
    idl_error(proc, &dir->data_type->location,
      "unknown data-type %s in keylist directive", data_type->identifier);
    return IDL_RETCODE_SEMANTIC_ERROR;
  }
  if (strcmp(data_type->identifier, entry->name->identifier) != 0) {
    idl_error(proc, &data_type->location,
      "data-type '%s' differs in case", data_type->identifier);
    return IDL_RETCODE_SEMANTIC_ERROR;
  }
  if (!idl_is_masked(entry->node, IDL_STRUCT) ||
       idl_is_masked(entry->node, IDL_FORWARD))
  {
    idl_error(proc, &data_type->location,
      "data-type %s in keylist directive is not a struct", data_type->identifier);
    return IDL_RETCODE_SEMANTIC_ERROR;
  }

  _struct = (idl_struct_t *)entry->node;
  for (size_t i=0; dir->keys && dir->keys[i]; i++) {
    idl_name_t *key = dir->keys[i];
    idl_member_t *member = _struct->members;
    for (; member; member = idl_next(member)) {
      idl_declarator_t *declarator = member->declarators;
      assert(declarator);
      for (; declarator; declarator = idl_next(declarator)) {
        if (strcmp(declarator->name->identifier, key->identifier) == 0)
          break;
      }
      if (declarator)
        break;
    }
    if (!member) {
      idl_error(proc, &key->location,
        "unknown struct member %s in keylist directive", key->identifier);
      return IDL_RETCODE_SEMANTIC_ERROR;
    } else if (idl_is_masked(member, IDL_KEY)) {
      idl_error(proc, &key->location,
        "redefinition of key %s in keylist directive", key->identifier);
      return IDL_RETCODE_SEMANTIC_ERROR;
    }
    member->node.mask |= IDL_KEY;
  }

  for (size_t i=0; dir->keys && dir->keys[i]; i++) {
    idl_name_t *key = dir->keys[i];
    free(key->identifier);
    free(key);
  }
  free(dir->keys);
  free(data_type->identifier);
  free(data_type);
  free(dir);
  proc->directive = NULL;
  return IDL_RETCODE_OK;
}

static int32_t
parse_keylist(idl_processor_t *proc, idl_token_t *tok)
{
  idl_keylist_t *dir = (idl_keylist_t *)proc->directive;
  assert(dir);

  /* #pragma keylist does not support scoped names */
  switch (proc->state) {
    case IDL_SCAN_KEYLIST: {
      idl_name_t *data_type;

      if (tok->code == '\n' || tok->code == '\0') {
        idl_error(proc, &tok->location,
          "no data-type in #pragma keylist directive");
        return IDL_RETCODE_SYNTAX_ERROR;
      } else if (tok->code != IDL_TOKEN_IDENTIFIER) {
        idl_error(proc, &tok->location,
          "invalid data-type in #pragma keylist directive");
        return IDL_RETCODE_SYNTAX_ERROR;
      }
      if (!(data_type = calloc(1, sizeof(*dir))))
        return IDL_RETCODE_NO_MEMORY;
      dir->data_type = data_type;
      data_type->location = tok->location;
      data_type->identifier = tok->value.str;
      /* do not free on return */
      tok->value.str = NULL;
      proc->state = IDL_SCAN_KEY;
    } break;
    case IDL_SCAN_KEY: {
      size_t nmemb = 0;
      idl_name_t *key, **keys;

      if (tok->code == '\n' || tok->code == '\0') {
        proc->state = IDL_SCAN;
        return push_keylist(proc, dir);
      } else if (tok->code == ',' && dir->keys) {
        /* #pragma keylist takes space or comma separated list of keys */
        break;
      } else if (tok->code != IDL_TOKEN_IDENTIFIER) {
        idl_error(proc, &tok->location,
          "invalid key in #pragma keylist directive");
        return IDL_RETCODE_SYNTAX_ERROR;
      } else if (idl_iskeyword(proc, tok->value.str, 1)) {
        idl_error(proc, &tok->location,
          "invalid key %s in #pragma keylist directive", tok->value.str);
        return IDL_RETCODE_SYNTAX_ERROR;
      }

      for (nmemb = 0; dir->keys && dir->keys[nmemb]; nmemb++) ;
      if (!(keys = realloc(dir->keys, (nmemb + 2) * sizeof(*keys))))
        return IDL_RETCODE_NO_MEMORY;
      dir->keys = keys;
      keys[nmemb+0] = NULL;
      if (!(key = malloc(sizeof(*key))))
        return IDL_RETCODE_NO_MEMORY;
      key->location = tok->location;
      key->identifier = tok->value.str;
      keys[nmemb+0] = key;
      keys[nmemb+1] = NULL;
      /* do not free on return */
      tok->value.str = NULL;
    } break;
    default:
      assert(0);
      break;
  }
  return 0;
}

idl_retcode_t idl_parse_directive(idl_processor_t *proc, idl_token_t *tok)
{
  /* order is important here */
  if ((proc->state & IDL_SCAN_LINE) == IDL_SCAN_LINE) {
    return parse_line(proc, tok);
  } else if ((proc->state & IDL_SCAN_KEYLIST) == IDL_SCAN_KEYLIST) {
    return parse_keylist(proc, tok);
  } else if (proc->state == IDL_SCAN_PRAGMA) {
    /* expect keylist */
    if (tok->code == IDL_TOKEN_IDENTIFIER) {
      if (strcmp(tok->value.str, "keylist") == 0) {
        idl_keylist_t *dir;
        if (!(dir = malloc(sizeof(*dir))))
          return IDL_RETCODE_NO_MEMORY;
        dir->symbol.mask = IDL_DIRECTIVE|IDL_KEYLIST;
        dir->symbol.location = tok->location;
        dir->data_type = NULL;
        dir->keys = NULL;
        proc->directive = (idl_symbol_t *)dir;
        proc->state = IDL_SCAN_KEYLIST;
        return 0;
      }
      idl_error(proc, &tok->location,
        "unsupported #pragma directive %s", tok->value.str);
      return IDL_RETCODE_SYNTAX_ERROR;
    }
  } else if (proc->state == IDL_SCAN_DIRECTIVE_NAME) {
    if (tok->code == IDL_TOKEN_IDENTIFIER) {
      /* expect line or pragma */
      if (strcmp(tok->value.str, "line") == 0) {
        idl_line_t *dir;
        if (!(dir = malloc(sizeof(*dir))))
          return IDL_RETCODE_NO_MEMORY;
        dir->symbol.mask = IDL_DIRECTIVE|IDL_LINE;
        dir->symbol.location = tok->location;
        dir->line = NULL;
        dir->file = NULL;
        dir->extra_tokens = false;
        proc->directive = (idl_symbol_t *)dir;
        proc->state = IDL_SCAN_LINE;
        return 0;
      } else if (strcmp(tok->value.str, "pragma") == 0) {
        /* support #pragma keylist for backwards compatibility */
        proc->state = IDL_SCAN_PRAGMA;
        return 0;
      }
    } else if (tok->code == '\n' || tok->code == '\0') {
      proc->state = IDL_SCAN;
      return 0;
    }
  } else if (proc->state == IDL_SCAN_DIRECTIVE) {
    /* expect # */
    if (tok->code == '#') {
      proc->state = IDL_SCAN_DIRECTIVE_NAME;
      return 0;
    }
  }

  idl_error(proc, &tok->location, "invalid compiler directive");
  return IDL_RETCODE_SYNTAX_ERROR;
}