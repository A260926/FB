/*
 * Copyright (C) 2026 Alexander Borisov
 *
 * Author: Alexander Borisov <borisov@lexbor.com>
 */

/*
 * Offline form entry replay.
 *
 * Parses two HTML forms shipped with the repository, applies a recorded
 * action sequence to the live DOM and extracts the selected radio button
 * name/value pairs of every form.
 *
 * The radio selection state is manipulated exclusively through the public
 * input checkedness/form owner API and the form reset API.
 *
 * Usage:
 *     form_replay <form_a.html> <form_b.html> <actions.txt>
 *
 * Action lines (blank lines and lines starting with '#' are ignored):
 *     <command> <document> <arguments...>
 *
 * Commands:
 *     select   <doc> <input-id>              select a radio button
 *     unselect <doc> <input-id>              clear the current checkedness
 *     name     <doc> <input-id> <name>       set the name attribute
 *     form     <doc> <input-id> <form-id>    set the form attribute
 *     move     <doc> <node-id> <parent-id>   move a node through the DOM
 *     attr     <doc> <node-id> <name> [val]  set/remove ("-") an attribute
 *     reset    <doc> <form-id>               reset a form
 *
 * Output, one record per line:
 *     ENTRY <form-id> <name>=<value>
 *     FORM  <form-id> empty
 *     REPLAY_OK
 *
 * Only radio button entries are exported; checkboxes, text values, select,
 * events and submission are intentionally out of scope.
 */

#include <lexbor/html/html.h>
#include <lexbor/dom/dom.h>
#include <lexbor/core/str.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define REPLAY_MAX_LINE 512

typedef struct {
    const char *name;
    lxb_html_document_t *document;
}
replay_doc_t;


#ifndef LXB_FORM_REPLAY_NO_MAIN
static int
replay_usage(const char *argv0)
{
    fprintf(stderr, "Usage: %s <form_a.html> <form_b.html> <actions.txt>\n",
            argv0);
    return 2;
}
#endif

static char *
replay_read_file(const char *path)
{
    long size;
    char *data;
    FILE *file;

    file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }

    size = ftell(file);
    if (size < 0) {
        fclose(file);
        return NULL;
    }

    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }

    data = (char *) malloc((size_t) size + 1);
    if (data == NULL) {
        fclose(file);
        return NULL;
    }

    if (size != 0 && fread(data, 1, (size_t) size, file) != (size_t) size) {
        free(data);
        fclose(file);
        return NULL;
    }

    data[size] = '\0';

    fclose(file);

    return data;
}

static lxb_html_document_t *
replay_parse(const char *path)
{
    char *html;
    lxb_status_t status;
    lxb_html_document_t *document;

    html = replay_read_file(path);
    if (html == NULL) {
        fprintf(stderr, "Failed to read %s\n", path);
        return NULL;
    }

    document = lxb_html_document_create();
    if (document == NULL) {
        free(html);
        return NULL;
    }

    status = lxb_html_document_parse(document, (const lxb_char_t *) html,
                                     strlen(html));
    free(html);

    if (status != LXB_STATUS_OK) {
        lxb_html_document_destroy(document);
        return NULL;
    }

    return document;
}

static lxb_dom_node_t *
replay_node_by_id(lxb_html_document_t *document, const char *id)
{
    return lxb_dom_node_by_id(lxb_dom_interface_node(document),
                              (const lxb_char_t *) id, strlen(id));
}

static lxb_html_document_t *
replay_document(replay_doc_t *docs, size_t length, const char *name)
{
    size_t i;

    for (i = 0; i < length; i++) {
        if (strcmp(docs[i].name, name) == 0) {
            return docs[i].document;
        }
    }

    return NULL;
}

static lxb_html_input_element_t *
replay_input(lxb_html_document_t *document, const char *id)
{
    lxb_dom_node_t *node;

    node = replay_node_by_id(document, id);
    if (node == NULL || node->type != LXB_DOM_NODE_TYPE_ELEMENT
        || node->local_name != LXB_TAG_INPUT)
    {
        return NULL;
    }

    return lxb_html_interface_input(node);
}

static lxb_html_form_element_t *
replay_form(lxb_html_document_t *document, const char *id)
{
    lxb_dom_node_t *node;

    node = replay_node_by_id(document, id);
    if (node == NULL || node->type != LXB_DOM_NODE_TYPE_ELEMENT
        || node->local_name != LXB_TAG_FORM)
    {
        return NULL;
    }

    return lxb_html_interface_form(node);
}

static bool
replay_select(lxb_html_document_t *document, const char *id)
{
    lxb_html_input_element_t *input = replay_input(document, id);

    if (input == NULL) {
        return false;
    }

    return lxb_html_input_element_set_checked(input, true) == LXB_STATUS_OK;
}

static bool
replay_unselect(lxb_html_document_t *document, const char *id)
{
    lxb_html_input_element_t *input = replay_input(document, id);

    if (input == NULL) {
        return false;
    }

    return lxb_html_input_element_set_checked(input, false) == LXB_STATUS_OK;
}

static bool
replay_set_attribute(lxb_html_document_t *document, const char *id,
                     const char *name, const char *value)
{
    lxb_dom_node_t *node = replay_node_by_id(document, id);

    if (node == NULL || node->type != LXB_DOM_NODE_TYPE_ELEMENT) {
        return false;
    }

    if (strcmp(value, "-") == 0) {
        return lxb_dom_element_remove_attribute(
                    lxb_dom_interface_element(node),
                    (const lxb_char_t *) name, strlen(name))
               == LXB_STATUS_OK;
    }

    return lxb_dom_element_set_attribute(lxb_dom_interface_element(node),
                                         (const lxb_char_t *) name,
                                         strlen(name),
                                         (const lxb_char_t *) value,
                                         strlen(value)) != NULL;
}

static bool
replay_move(lxb_html_document_t *document, const char *id, const char *target)
{
    lxb_dom_node_t *node, *parent;

    node = replay_node_by_id(document, id);
    parent = replay_node_by_id(document, target);

    if (node == NULL || parent == NULL) {
        return false;
    }

    /* Moves through the DOM entry point; mutation steps update the state. */
    return lxb_dom_node_append_child(parent, node)
           == LXB_DOM_EXCEPTION_OK;
}

static bool
replay_reset(lxb_html_document_t *document, const char *id)
{
    lxb_html_form_element_t *form = replay_form(document, id);

    if (form == NULL) {
        return false;
    }

    return lxb_html_form_element_reset(form) == LXB_STATUS_OK;
}

static bool
replay_apply(lxb_html_document_t *document, char *line)
{
    char *command, *id, *arg, *arg2;

    command = strtok(line, " \t\r\n");
    id = strtok(NULL, " \t\r\n");

    if (command == NULL || id == NULL) {
        return false;
    }

    if (strcmp(command, "select") == 0) {
        return replay_select(document, id);
    }

    if (strcmp(command, "unselect") == 0) {
        return replay_unselect(document, id);
    }

    if (strcmp(command, "reset") == 0) {
        return replay_reset(document, id);
    }

    if (strcmp(command, "move") == 0) {
        arg = strtok(NULL, " \t\r\n");
        return arg != NULL && replay_move(document, id, arg);
    }

    if (strcmp(command, "name") == 0 || strcmp(command, "form") == 0) {
        arg = strtok(NULL, " \t\r\n");
        return arg != NULL && replay_set_attribute(document, id, command, arg);
    }

    if (strcmp(command, "attr") == 0) {
        arg = strtok(NULL, " \t\r\n");
        arg2 = strtok(NULL, " \t\r\n");

        if (arg == NULL) {
            return false;
        }

        if (arg2 == NULL) {
            arg2 = (char *) "-";
        }

        return replay_set_attribute(document, id, arg, arg2);
    }

    return false;
}

static bool
replay_is_radio(lxb_html_input_element_t *input)
{
    lxb_dom_attr_t *attr;
    const lxb_char_t *value;
    size_t length;

    attr = lxb_dom_element_attr_by_id(lxb_dom_interface_element(
                lxb_dom_interface_node(input)), LXB_DOM_ATTR_TYPE);
    if (attr == NULL) {
        return false;
    }

    value = lxb_dom_attr_value(attr, &length);

    return value != NULL && length == 5
           && lexbor_str_data_ncasecmp(value,
                                       (const lxb_char_t *) "radio", 5);
}

/*
 * Export the selected radio buttons of a form in tree order.
 */
typedef struct {
    lxb_html_form_element_t *form;
    const char              *form_id;
    FILE                    *out;
    unsigned                count;
}
replay_export_t;

static lexbor_action_t
replay_export_walker(lxb_dom_node_t *node, void *ctx)
{
    replay_export_t *state = (replay_export_t *) ctx;
    lxb_html_input_element_t *input;
    lxb_dom_attr_t *name_attr, *value_attr;
    const lxb_char_t *name, *value;
    size_t name_len, value_len;

    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT
        || node->local_name != LXB_TAG_INPUT
        || node->ns != LXB_NS_HTML)
    {
        return LEXBOR_ACTION_OK;
    }

    input = lxb_html_interface_input(node);

    if (lxb_html_input_element_form_owner(input) != state->form
        || !lxb_html_input_element_checked(input)
        || !replay_is_radio(input))
    {
        return LEXBOR_ACTION_OK;
    }

    name_attr = lxb_dom_element_attr_by_name(lxb_dom_interface_element(node),
                                             (const lxb_char_t *) "name", 4);
    value_attr = lxb_dom_element_attr_by_name(lxb_dom_interface_element(node),
                                              (const lxb_char_t *) "value", 5);

    if (name_attr == NULL || value_attr == NULL) {
        return LEXBOR_ACTION_OK;
    }

    name = lxb_dom_attr_value(name_attr, &name_len);
    value = lxb_dom_attr_value(value_attr, &value_len);

    if (name == NULL || value == NULL || name_len == 0) {
        return LEXBOR_ACTION_OK;
    }

    fprintf(state->out, "ENTRY %s %.*s=%.*s\n", state->form_id,
            (int) name_len, (const char *) name, (int) value_len,
            (const char *) value);

    state->count++;

    return LEXBOR_ACTION_OK;
}

static unsigned
replay_export_form(lxb_html_document_t *document, const char *form_id,
                   FILE *out)
{
    replay_export_t state;
    lxb_html_form_element_t *form;

    form = replay_form(document, form_id);
    if (form == NULL) {
        return 0;
    }

    state.form = form;
    state.form_id = form_id;
    state.out = out;
    state.count = 0;

    lxb_dom_node_simple_walk(lxb_dom_interface_node(document),
                             replay_export_walker, &state);

    return state.count;
}

static void
replay_export_documents(replay_doc_t *docs, size_t length, FILE *out)
{
    size_t i, j, collection_length;
    lxb_dom_collection_t *collection;
    lxb_html_form_element_t *form;
    lxb_dom_attr_t *attr;
    const lxb_char_t *id;
    size_t id_len;
    char form_id[64];

    for (i = 0; i < length; i++) {
        collection = lxb_dom_collection_make(&docs[i].document->dom_document,
                                             8);
        if (collection == NULL) {
            continue;
        }

        if (lxb_dom_elements_by_tag_name(
                lxb_dom_interface_element(docs[i].document), collection,
                (const lxb_char_t *) "form", 4) != LXB_STATUS_OK)
        {
            lxb_dom_collection_destroy(collection, true);
            continue;
        }

        collection_length = lxb_dom_collection_length(collection);

        for (j = 0; j < collection_length; j++) {
            form = lxb_html_interface_form(
                       lxb_dom_collection_node(collection, j));

            attr = lxb_dom_element_attr_by_id(lxb_dom_interface_element(form),
                                              LXB_DOM_ATTR_ID);
            if (attr == NULL) {
                continue;
            }

            id = lxb_dom_attr_value(attr, &id_len);
            if (id == NULL || id_len >= sizeof(form_id)) {
                continue;
            }

            memcpy(form_id, id, id_len);
            form_id[id_len] = '\0';

            if (replay_export_form(docs[i].document, form_id, out) == 0) {
                fprintf(out, "FORM %s empty\n", form_id);
            }
        }

        lxb_dom_collection_destroy(collection, true);
    }
}

/*
 * Run a replay from an in-memory action transcript and write the structured
 * records to out.  Returns EXIT_SUCCESS on success.  Shared by the command
 * line tool and by the contract tests.
 */
int
lxb_form_replay_run(const char *form_a_path, const char *form_b_path,
                    const char *actions_text, FILE *out)
{
    int status = EXIT_SUCCESS;
    char *line, *next, *transcript;
    size_t document_count = 2;
    lxb_html_document_t *document;
    replay_doc_t docs[2];
    unsigned line_number = 0;

    transcript = (char *) malloc(strlen(actions_text) + 1);
    if (transcript == NULL) {
        return EXIT_FAILURE;
    }

    memcpy(transcript, actions_text, strlen(actions_text) + 1);

    docs[0].name = "A";
    docs[0].document = NULL;
    docs[1].name = "B";
    docs[1].document = NULL;

    docs[0].document = replay_parse(form_a_path);
    docs[1].document = replay_parse(form_b_path);

    if (docs[0].document == NULL || docs[1].document == NULL) {
        status = EXIT_FAILURE;
        goto done;
    }

    line = transcript;

    while (line != NULL && *line != '\0') {
        char *trim;

        next = strchr(line, '\n');

        if (next != NULL) {
            *next = '\0';
            next++;
        }

        line_number++;

        trim = line;

        while (*trim == ' ' || *trim == '\t') {
            trim++;
        }

        if (*trim == '\0' || *trim == '#') {
            line = next;
            continue;
        }

        {
            char *copy, *command, *doc_name, *args;
            char merged[REPLAY_MAX_LINE];

            copy = (char *) malloc(strlen(trim) + 1);
            if (copy == NULL) {
                status = EXIT_FAILURE;
                break;
            }

            memcpy(copy, trim, strlen(trim) + 1);

            /* Action line: <command> <document> <arguments...>. */
            command = strtok(copy, " \t\r\n");
            doc_name = strtok(NULL, " \t\r\n");
            args = strtok(NULL, "\r\n");

            if (command == NULL || doc_name == NULL) {
                fprintf(stderr, "Bad action at line %u\n", line_number);
                free(copy);
                status = EXIT_FAILURE;
                break;
            }

            if (args == NULL) {
                args = (char *) "";
            }

            snprintf(merged, sizeof(merged), "%s %s", command, args);

            document = replay_document(docs, document_count, doc_name);

            if (document == NULL || !replay_apply(document, merged)) {
                fprintf(stderr, "Failed action at line %u: %s %s\n",
                        line_number, doc_name, merged);
                free(copy);
                status = EXIT_FAILURE;
                break;
            }

            free(copy);
        }

        line = next;
    }

    if (status == EXIT_SUCCESS) {
        replay_export_documents(docs, document_count, out);
        fprintf(out, "REPLAY_OK\n");
    }

done:

    free(transcript);

    lxb_html_document_destroy(docs[0].document);
    lxb_html_document_destroy(docs[1].document);

    return status;
}

#ifndef LXB_FORM_REPLAY_NO_MAIN

int
main(int argc, const char *argv[])
{
    int status;
    char *actions;

    if (argc != 4) {
        return replay_usage(argv[0]);
    }

    actions = replay_read_file(argv[3]);
    if (actions == NULL) {
        fprintf(stderr, "Failed to read %s\n", argv[3]);
        return EXIT_FAILURE;
    }

    status = lxb_form_replay_run(argv[1], argv[2], actions, stdout);

    free(actions);

    return status;
}

#endif /* LXB_FORM_REPLAY_NO_MAIN */
