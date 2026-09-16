/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "claw_skill.h"
#include "cap_skill_mgr.h"
#include "esp_log.h"

static const char *TAG = "cap_skill_mgr";
static const char *CAP_SKILL_LIST = "list_skill";
static const char *CAP_SKILL_CREATE = "create_skill";
static const char *CAP_SKILL_REGISTER = "register_skill";
static const char *CAP_SKILL_UNREGISTER = "unregister_skill";

#define CAP_SKILL_MAX_CATALOG_LEN   16384
#define CAP_SKILL_MAX_PATH_LEN      128
#define CAP_SKILL_MAX_ID_LEN        63
#define CAP_SKILL_MAX_MARKDOWN_LEN  (20 * 1024)

static char s_skill_root_dir[CAP_SKILL_MAX_PATH_LEN];

static const char *cap_skill_root_dir(void)
{
    return s_skill_root_dir[0] ? s_skill_root_dir : NULL;
}

static void cap_skill_free_string_array(char **items, size_t count)
{
    size_t i;

    if (!items) {
        return;
    }

    for (i = 0; i < count; i++) {
        free(items[i]);
    }
    free(items);
}

static esp_err_t cap_skill_sync_session_visible_groups(const char *session_id)
{
    char **group_ids = NULL;
    size_t group_count = 0;
    esp_err_t err = ESP_OK;

    if (!session_id || !session_id[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    err = claw_skill_load_active_cap_groups(session_id, &group_ids, &group_count);
    if (err == ESP_ERR_NOT_FOUND) {
        return claw_cap_set_session_llm_visible_groups(session_id, NULL, 0);
    }
    if (err != ESP_OK) {
        return err;
    }

    err = claw_cap_set_session_llm_visible_groups(session_id,
                                                   (const char *const *)group_ids,
                                                   group_count);
    cap_skill_free_string_array(group_ids, group_count);
    return err;
}

static void cap_skill_write_error(char *output,
                                  size_t output_size,
                                  const char *error,
                                  const char *skill_id)
{
    cJSON *root = NULL;
    char *rendered = NULL;

    if (!output || output_size == 0) {
        return;
    }

    root = cJSON_CreateObject();
    if (!root) {
        snprintf(output,
                 output_size,
                 "{\"ok\":false,\"error\":\"%s\"}",
                 error ? error : "unknown error");
        return;
    }

    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", error ? error : "unknown error");
    if (skill_id && skill_id[0]) {
        cJSON_AddStringToObject(root, "skill_id", skill_id);
    }

    rendered = cJSON_PrintUnformatted(root);
    if (rendered) {
        snprintf(output, output_size, "%s", rendered);
        free(rendered);
    } else {
        snprintf(output,
                 output_size,
                 "{\"ok\":false,\"error\":\"%s\"}",
                 error ? error : "unknown error");
    }
    cJSON_Delete(root);
}

static esp_err_t cap_skill_read_file_dup(const char *path, char **out_text)
{
    FILE *file = NULL;
    long size;
    char *text = NULL;
    size_t read_bytes;

    if (!path || !out_text) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_text = NULL;

    file = fopen(path, "rb");
    if (!file) {
        return ESP_ERR_NOT_FOUND;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return ESP_FAIL;
    }
    size = ftell(file);
    if (size < 0 || size > CAP_SKILL_MAX_MARKDOWN_LEN) {
        fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return ESP_FAIL;
    }

    text = calloc(1, (size_t)size + 1);
    if (!text) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }
    read_bytes = fread(text, 1, (size_t)size, file);
    if (read_bytes != (size_t)size && ferror(file)) {
        fclose(file);
        free(text);
        return ESP_FAIL;
    }
    fclose(file);
    text[read_bytes] = '\0';
    *out_text = text;
    return ESP_OK;
}

static esp_err_t cap_skill_write_file_text(const char *path, const char *text)
{
    FILE *file = NULL;
    size_t text_len;

    if (!path || !text) {
        return ESP_ERR_INVALID_ARG;
    }

    text_len = strlen(text);
    file = fopen(path, "wb");
    if (!file) {
        return ESP_FAIL;
    }
    if (text_len > 0 && fwrite(text, 1, text_len, file) != text_len) {
        fclose(file);
        return ESP_FAIL;
    }
    if (fflush(file) != 0) {
        fclose(file);
        return ESP_FAIL;
    }
    if (fclose(file) != 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static bool cap_skill_id_is_valid(const char *skill_id)
{
    size_t i;
    size_t len;

    if (!skill_id || !skill_id[0]) {
        return false;
    }

    len = strlen(skill_id);
    if (len == 0 || len > CAP_SKILL_MAX_ID_LEN) {
        return false;
    }

    for (i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)skill_id[i];

        if (!(isalnum(ch) || ch == '_' || ch == '-')) {
            return false;
        }
    }
    return true;
}

static bool cap_skill_path_is_valid(const char *skill_id, const char *path)
{
    char expected[CAP_SKILL_MAX_PATH_LEN];

    if (!cap_skill_id_is_valid(skill_id) || !path || !path[0]) {
        return false;
    }
    if (path[0] == '/' || strstr(path, "..") != NULL || strchr(path, '\\') != NULL) {
        return false;
    }
    if (snprintf(expected, sizeof(expected), "%s/SKILL.md", skill_id) >= (int)sizeof(expected)) {
        return false;
    }
    return strcmp(path, expected) == 0;
}

static bool cap_skill_file_exists(const char *path)
{
    struct stat st = {0};

    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static esp_err_t cap_skill_ensure_dir(const char *path, bool *out_created)
{
    struct stat st = {0};

    if (out_created) {
        *out_created = false;
    }
    if (!path || !path[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? ESP_OK : ESP_ERR_INVALID_STATE;
    }

    errno = 0;
    if (mkdir(path, 0755) == 0) {
        if (out_created) {
            *out_created = true;
        }
        return ESP_OK;
    }
    if (errno == EEXIST && stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        return ESP_OK;
    }
    return ESP_FAIL;
}

static esp_err_t cap_skill_build_runtime_paths(const char *skill_id,
                                               char *skill_dir,
                                               size_t skill_dir_size,
                                               char *skill_path,
                                               size_t skill_path_size,
                                               char *relative_path,
                                               size_t relative_path_size)
{
    const char *root_dir = cap_skill_root_dir();

    if (!root_dir || !cap_skill_id_is_valid(skill_id) ||
            !skill_dir || !skill_path || !relative_path) {
        return ESP_ERR_INVALID_ARG;
    }

    if (snprintf(relative_path, relative_path_size, "%s/SKILL.md", skill_id) >=
            (int)relative_path_size ||
            snprintf(skill_dir, skill_dir_size, "%s/%s", root_dir, skill_id) >=
            (int)skill_dir_size ||
            snprintf(skill_path, skill_path_size, "%s/%s", root_dir, relative_path) >=
            (int)skill_path_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static void cap_skill_restore_file(const char *skill_path,
                                   const char *old_markdown,
                                   bool had_old_file,
                                   const char *skill_dir,
                                   bool created_dir)
{
    if (!skill_path) {
        return;
    }

    if (had_old_file && old_markdown) {
        if (cap_skill_write_file_text(skill_path, old_markdown) != ESP_OK) {
            ESP_LOGE(TAG, "failed to restore skill markdown %s", skill_path);
        }
    } else {
        (void)remove(skill_path);
        if (created_dir && skill_dir) {
            (void)rmdir(skill_dir);
        }
    }

    /* Restore the in-memory view as well. Ignore the return here because this
     * helper is already running on an error path and the primary error is more
     * useful to the caller. */
    (void)claw_skill_reload_registry();
}

static esp_err_t cap_skill_load_catalog_json(char **out_text, cJSON **out_catalog)
{
    char *catalog_text = NULL;
    cJSON *catalog = NULL;
    esp_err_t err;

    if (!out_text || !out_catalog) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_text = NULL;
    *out_catalog = NULL;

    catalog_text = calloc(1, CAP_SKILL_MAX_CATALOG_LEN);
    if (!catalog_text) {
        return ESP_ERR_NO_MEM;
    }

    err = claw_skill_render_catalog_json(catalog_text, CAP_SKILL_MAX_CATALOG_LEN);
    if (err != ESP_OK) {
        free(catalog_text);
        return err;
    }

    catalog = cJSON_Parse(catalog_text);
    if (!catalog || !cJSON_IsObject(catalog)) {
        cJSON_Delete(catalog);
        free(catalog_text);
        return ESP_ERR_INVALID_STATE;
    }

    *out_text = catalog_text;
    *out_catalog = catalog;
    return ESP_OK;
}

static const char *cap_skill_manage_mode_to_string(claw_skill_manage_mode_t mode)
{
    switch (mode) {
    case CLAW_SKILL_MANAGE_MODE_READONLY:
        return "readonly";
    case CLAW_SKILL_MANAGE_MODE_RUNTIME:
        return "runtime";
    default:
        return "unknown";
    }
}

static cJSON *cap_skill_catalog_entry_to_json(const claw_skill_catalog_entry_t *entry)
{
    cJSON *skill = NULL;
    cJSON *cap_groups = NULL;
    size_t i;

    if (!entry) {
        return NULL;
    }

    skill = cJSON_CreateObject();
    cap_groups = cJSON_CreateArray();
    if (!skill || !cap_groups) {
        cJSON_Delete(skill);
        cJSON_Delete(cap_groups);
        return NULL;
    }

    cJSON_AddStringToObject(skill, "id", entry->id ? entry->id : "");
    cJSON_AddStringToObject(skill, "file", entry->file ? entry->file : "");
    cJSON_AddStringToObject(skill, "summary", entry->summary ? entry->summary : "");
    cJSON_AddStringToObject(skill, "manage_mode", cap_skill_manage_mode_to_string(entry->manage_mode));
    for (i = 0; i < entry->cap_group_count; i++) {
        cJSON_AddItemToArray(cap_groups, cJSON_CreateString(entry->cap_groups[i]));
    }
    cJSON_AddItemToObject(skill, "cap_groups", cap_groups);
    return skill;
}

static esp_err_t cap_skill_build_catalog_result(const char *action,
                                                cJSON *skill,
                                                const char *skill_id,
                                                char *output,
                                                size_t output_size)
{
    cJSON *root = NULL;
    cJSON *catalog = NULL;
    cJSON *skills = NULL;
    char *catalog_text = NULL;
    char *rendered = NULL;
    esp_err_t err;

    if (!action || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    err = cap_skill_load_catalog_json(&catalog_text, &catalog);
    if (err != ESP_OK) {
        cJSON_Delete(skill);
        return err;
    }
    free(catalog_text);

    skills = cJSON_DetachItemFromObjectCaseSensitive(catalog, "skills");
    cJSON_Delete(catalog);
    if (!cJSON_IsArray(skills)) {
        cJSON_Delete(skills);
        cJSON_Delete(skill);
        return ESP_ERR_INVALID_STATE;
    }

    root = cJSON_CreateObject();
    if (!root) {
        cJSON_Delete(skills);
        cJSON_Delete(skill);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "action", action);
    if (skill) {
        cJSON_AddItemToObject(root, "skill", skill);
    } else if (skill_id && skill_id[0]) {
        cJSON_AddStringToObject(root, "skill_id", skill_id);
    }
    cJSON_AddItemToObject(root, "skills", skills);

    rendered = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!rendered) {
        return ESP_ERR_NO_MEM;
    }

    snprintf(output, output_size, "%s", rendered);
    free(rendered);
    return ESP_OK;
}

static esp_err_t cap_skill_build_create_result(const claw_skill_catalog_entry_t *entry,
                                               bool created,
                                               bool activated,
                                               const char *warning,
                                               char *output,
                                               size_t output_size)
{
    cJSON *root = NULL;
    cJSON *skill = NULL;
    char *rendered = NULL;

    if (!entry || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    root = cJSON_CreateObject();
    skill = cap_skill_catalog_entry_to_json(entry);
    if (!root || !skill) {
        cJSON_Delete(root);
        cJSON_Delete(skill);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "action", CAP_SKILL_CREATE);
    cJSON_AddBoolToObject(root, "created", created);
    cJSON_AddBoolToObject(root, "updated", !created);
    cJSON_AddBoolToObject(root, "activated", activated);
    cJSON_AddItemToObject(root, "skill", skill);
    if (warning && warning[0]) {
        cJSON_AddStringToObject(root, "warning", warning);
    }

    rendered = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!rendered) {
        return ESP_ERR_NO_MEM;
    }
    if (strlen(rendered) >= output_size) {
        free(rendered);
        return ESP_ERR_INVALID_SIZE;
    }

    snprintf(output, output_size, "%s", rendered);
    free(rendered);
    return ESP_OK;
}

static esp_err_t cap_skill_activate_execute(const char *input_json,
                                            const claw_cap_call_context_t *ctx,
                                            char *output,
                                            size_t output_size)
{
    cJSON *root = NULL;
    cJSON *skill_id_item = NULL;
    char *doc_text = NULL;
    char activated_skill_id[64] = {0};
    const char *prefix = "<skill_content name=\"";
    const char *middle = "\">\n";
    const char *suffix = "\n</skill_content>";
    size_t content_len;
    int written;
    esp_err_t err = ESP_OK;

    if (!ctx || !ctx->session_id || !ctx->session_id[0] || !output || output_size == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    root = cJSON_Parse(input_json ? input_json : "{}");
    if (!root) {
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"invalid input json\"}");
        return ESP_ERR_INVALID_ARG;
    }
    skill_id_item = cJSON_GetObjectItemCaseSensitive(root, "skill_id");

    if (!cJSON_IsString(skill_id_item) || !skill_id_item->valuestring || !skill_id_item->valuestring[0]) {
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"skill_id is required\"}");
        err = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }
    if (!cap_skill_id_is_valid(skill_id_item->valuestring)) {
        cap_skill_write_error(output, output_size, "invalid skill_id", skill_id_item->valuestring);
        err = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }
    if (strlen(skill_id_item->valuestring) >= sizeof(activated_skill_id)) {
        cap_skill_write_error(output, output_size, "skill_id is too long", NULL);
        err = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }

    snprintf(activated_skill_id, sizeof(activated_skill_id), "%s", skill_id_item->valuestring);

    doc_text = calloc(1, output_size);
    if (!doc_text) {
        cap_skill_write_error(output, output_size, "out of memory", NULL);
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    err = claw_skill_read_document(activated_skill_id, doc_text, output_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to read skill doc %s: %s",
                 activated_skill_id, esp_err_to_name(err));
        cap_skill_write_error(output, output_size, "failed to read skill document", activated_skill_id);
        goto cleanup;
    }

    content_len = strlen(prefix) + strlen(activated_skill_id) + strlen(middle) +
                  strlen(doc_text) + strlen(suffix);
    if (content_len >= output_size) {
        ESP_LOGE(TAG, "skill content result too large: %u >= %u",
                 (unsigned)content_len, (unsigned)output_size);
        cap_skill_write_error(output, output_size, "skill content result too large", activated_skill_id);
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    err = claw_skill_activate_for_session(ctx->session_id, activated_skill_id);
    if (err != ESP_OK) {
        cap_skill_write_error(output, output_size, "failed to activate skill", activated_skill_id);
        goto cleanup;
    }

    err = cap_skill_sync_session_visible_groups(ctx->session_id);
    if (err != ESP_OK) {
        cap_skill_write_error(output, output_size, "failed to sync capability visibility", activated_skill_id);
        goto cleanup;
    }

    written = snprintf(output, output_size, "%s%s%s%s%s",
                       prefix, activated_skill_id, middle, doc_text, suffix);
    if (written < 0 || (size_t)written >= output_size) {
        cap_skill_write_error(output, output_size, "skill content result too large", activated_skill_id);
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

cleanup:
    cJSON_Delete(root);
    free(doc_text);
    return err;
}

static esp_err_t cap_skill_list_execute(const char *input_json,
                                        const claw_cap_call_context_t *ctx,
                                        char *output,
                                        size_t output_size)
{
    (void)input_json;
    (void)ctx;

    return cap_skill_build_catalog_result(CAP_SKILL_LIST, NULL, NULL, output, output_size);
}

/*
 * High-level runtime Skill creation.
 *
 * This intentionally combines file creation, registry reload, verification and
 * optional activation in one capability call. The model therefore does not
 * need to orchestrate a fragile write_file -> register_skill -> activate_skill
 * sequence for ordinary runtime Skill creation.
 */
static esp_err_t cap_skill_create_execute(const char *input_json,
                                          const claw_cap_call_context_t *ctx,
                                          char *output,
                                          size_t output_size)
{
    char skill_id[CAP_SKILL_MAX_ID_LEN + 1] = {0};
    char skill_dir[CAP_SKILL_MAX_PATH_LEN];
    char skill_path[CAP_SKILL_MAX_PATH_LEN];
    char relative_path[CAP_SKILL_MAX_PATH_LEN];
    char *old_markdown = NULL;
    const char *markdown = NULL;
    const char *warning = NULL;
    cJSON *root = NULL;
    cJSON *skill_id_item = NULL;
    cJSON *markdown_item = NULL;
    cJSON *activate_item = NULL;
    cJSON *overwrite_item = NULL;
    claw_skill_catalog_entry_t existing_entry = {0};
    claw_skill_catalog_entry_t entry = {0};
    bool catalog_entry_exists = false;
    bool had_old_file = false;
    bool created_dir = false;
    bool activate = true;
    bool overwrite = false;
    bool activated = false;
    esp_err_t err;

    if (!ctx || !ctx->session_id || !ctx->session_id[0] || !output || output_size == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    root = cJSON_Parse(input_json ? input_json : "{}");
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        cap_skill_write_error(output, output_size, "invalid input json", NULL);
        return ESP_ERR_INVALID_ARG;
    }

    skill_id_item = cJSON_GetObjectItemCaseSensitive(root, "skill_id");
    markdown_item = cJSON_GetObjectItemCaseSensitive(root, "markdown");
    activate_item = cJSON_GetObjectItemCaseSensitive(root, "activate");
    overwrite_item = cJSON_GetObjectItemCaseSensitive(root, "overwrite");

    if (!cJSON_IsString(skill_id_item) || !skill_id_item->valuestring ||
            !cap_skill_id_is_valid(skill_id_item->valuestring)) {
        cap_skill_write_error(output, output_size,
                              "skill_id must contain only letters, digits, '_' or '-' and be at most 63 characters",
                              NULL);
        err = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }
    if (!cJSON_IsString(markdown_item) || !markdown_item->valuestring || !markdown_item->valuestring[0]) {
        cap_skill_write_error(output, output_size, "markdown is required", skill_id_item->valuestring);
        err = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }
    if (activate_item && !cJSON_IsBool(activate_item)) {
        cap_skill_write_error(output, output_size, "activate must be boolean", skill_id_item->valuestring);
        err = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }
    if (overwrite_item && !cJSON_IsBool(overwrite_item)) {
        cap_skill_write_error(output, output_size, "overwrite must be boolean", skill_id_item->valuestring);
        err = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }

    activate = activate_item ? cJSON_IsTrue(activate_item) : true;
    overwrite = overwrite_item ? cJSON_IsTrue(overwrite_item) : false;
    markdown = markdown_item->valuestring;

    if (strlen(markdown) > CAP_SKILL_MAX_MARKDOWN_LEN) {
        cap_skill_write_error(output, output_size, "skill markdown exceeds 20 KiB", skill_id_item->valuestring);
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    strlcpy(skill_id, skill_id_item->valuestring, sizeof(skill_id));

    err = cap_skill_build_runtime_paths(skill_id,
                                        skill_dir,
                                        sizeof(skill_dir),
                                        skill_path,
                                        sizeof(skill_path),
                                        relative_path,
                                        sizeof(relative_path));
    if (err != ESP_OK) {
        cap_skill_write_error(output, output_size, "skill path is too long or storage is unavailable", skill_id);
        goto cleanup;
    }

    err = claw_skill_get_catalog_entry(skill_id, &existing_entry);
    if (err == ESP_OK) {
        catalog_entry_exists = true;
        if (existing_entry.manage_mode == CLAW_SKILL_MANAGE_MODE_READONLY) {
            cap_skill_write_error(output, output_size, "cannot overwrite a readonly system skill", skill_id);
            err = ESP_ERR_INVALID_STATE;
            goto cleanup;
        }
        if (!overwrite) {
            cap_skill_write_error(output, output_size,
                                  "runtime skill already exists; set overwrite=true to update it",
                                  skill_id);
            err = ESP_ERR_INVALID_STATE;
            goto cleanup;
        }
    } else if (err != ESP_ERR_NOT_FOUND) {
        cap_skill_write_error(output, output_size, "failed to inspect existing skill", skill_id);
        goto cleanup;
    }

    had_old_file = cap_skill_file_exists(skill_path);
    if (had_old_file && !catalog_entry_exists && !overwrite) {
        cap_skill_write_error(output, output_size,
                              "skill file already exists but is not registered; set overwrite=true to repair it",
                              skill_id);
        err = ESP_ERR_INVALID_STATE;
        goto cleanup;
    }

    if (had_old_file) {
        err = cap_skill_read_file_dup(skill_path, &old_markdown);
        if (err != ESP_OK) {
            cap_skill_write_error(output, output_size, "failed to back up existing skill markdown", skill_id);
            goto cleanup;
        }
    }

    err = cap_skill_ensure_dir(skill_dir, &created_dir);
    if (err != ESP_OK) {
        cap_skill_write_error(output, output_size, "failed to create skill directory", skill_id);
        goto cleanup;
    }

    err = cap_skill_write_file_text(skill_path, markdown);
    if (err != ESP_OK) {
        cap_skill_restore_file(skill_path, old_markdown, had_old_file, skill_dir, created_dir);
        cap_skill_write_error(output, output_size, "failed to write skill markdown", skill_id);
        goto cleanup;
    }

    err = claw_skill_reload_registry();
    if (err != ESP_OK) {
        cap_skill_restore_file(skill_path, old_markdown, had_old_file, skill_dir, created_dir);
        cap_skill_write_error(output, output_size, "skill markdown is invalid or registry reload failed", skill_id);
        goto cleanup;
    }

    err = claw_skill_get_catalog_entry(skill_id, &entry);
    if (err != ESP_OK || !entry.file || strcmp(entry.file, relative_path) != 0 ||
            entry.manage_mode != CLAW_SKILL_MANAGE_MODE_RUNTIME) {
        cap_skill_restore_file(skill_path, old_markdown, had_old_file, skill_dir, created_dir);
        if (err == ESP_OK) {
            err = ESP_ERR_INVALID_STATE;
        }
        cap_skill_write_error(output, output_size,
                              "skill did not verify after registry reload; check SKILL.md frontmatter and id",
                              skill_id);
        goto cleanup;
    }

    /* Activation is intentionally best-effort once the Skill itself has been
     * created and verified. A transient session-state failure must not cause
     * the model to repeat file creation and potentially enter another tool
     * loop. */
    if (activate) {
        esp_err_t activate_err = claw_skill_activate_for_session(ctx->session_id, skill_id);

        if (activate_err == ESP_OK) {
            activated = true;
            if (cap_skill_sync_session_visible_groups(ctx->session_id) != ESP_OK) {
                warning = "skill was activated but capability visibility sync failed; retry activation if needed";
            }
        } else {
            warning = "skill was created and registered but activation failed; call activate_skill to retry";
        }
    }

    err = cap_skill_build_create_result(&entry,
                                        !had_old_file,
                                        activated,
                                        warning,
                                        output,
                                        output_size);
    if (err != ESP_OK) {
        cap_skill_write_error(output, output_size, "skill created but result serialization failed", skill_id);
    }

cleanup:
    cJSON_Delete(root);
    free(old_markdown);
    return err;
}

static esp_err_t cap_skill_register_execute(const char *input_json,
                                            const claw_cap_call_context_t *ctx,
                                            char *output,
                                            size_t output_size)
{
    char skill_path[CAP_SKILL_MAX_PATH_LEN];
    cJSON *root = NULL;
    cJSON *skill_id_item = NULL;
    cJSON *file_item = NULL;
    cJSON *skill = NULL;
    claw_skill_catalog_entry_t entry;
    esp_err_t err;

    (void)ctx;

    root = cJSON_Parse(input_json ? input_json : "{}");
    if (!root) {
        cap_skill_write_error(output, output_size, "invalid input json", NULL);
        return ESP_ERR_INVALID_ARG;
    }

    skill_id_item = cJSON_GetObjectItemCaseSensitive(root, "skill_id");
    file_item = cJSON_GetObjectItemCaseSensitive(root, "file");
    if (!cJSON_IsString(skill_id_item) || !skill_id_item->valuestring || !skill_id_item->valuestring[0] ||
            !cJSON_IsString(file_item) || !file_item->valuestring || !file_item->valuestring[0]) {
        cJSON_Delete(root);
        cap_skill_write_error(output, output_size, "skill_id and file are required", NULL);
        return ESP_ERR_INVALID_ARG;
    }

    if (!cap_skill_path_is_valid(skill_id_item->valuestring, file_item->valuestring)) {
        cJSON_Delete(root);
        cap_skill_write_error(output, output_size, "file must be <skill_id>/SKILL.md", skill_id_item->valuestring);
        return ESP_ERR_INVALID_ARG;
    }

    {
        const char *root_dir = cap_skill_root_dir();
        if (!root_dir) {
            cJSON_Delete(root);
            cap_skill_write_error(output, output_size, "skill storage is not initialized", skill_id_item->valuestring);
            return ESP_ERR_INVALID_STATE;
        }
        if (snprintf(skill_path, sizeof(skill_path), "%s/%s", root_dir, file_item->valuestring) >= (int)sizeof(skill_path)) {
            cJSON_Delete(root);
            cap_skill_write_error(output, output_size, "file path is too long", skill_id_item->valuestring);
            return ESP_ERR_INVALID_SIZE;
        }
    }
    if (!cap_skill_file_exists(skill_path)) {
        cJSON_Delete(root);
        cap_skill_write_error(output, output_size, "skill markdown file does not exist", skill_id_item->valuestring);
        return ESP_ERR_NOT_FOUND;
    }

    err = claw_skill_reload_registry();
    if (err != ESP_OK) {
        cJSON_Delete(root);
        cap_skill_write_error(output, output_size, "failed to reload skill registry", skill_id_item->valuestring);
        return err;
    }

    err = claw_skill_get_catalog_entry(skill_id_item->valuestring, &entry);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        cap_skill_write_error(output, output_size, "skill not found after registry reload", skill_id_item->valuestring);
        return err;
    }
    if (!entry.file || strcmp(entry.file, file_item->valuestring) != 0) {
        cJSON_Delete(root);
        cap_skill_write_error(output, output_size, "registered skill file does not match requested file", skill_id_item->valuestring);
        return ESP_ERR_INVALID_STATE;
    }

    skill = cap_skill_catalog_entry_to_json(&entry);
    if (!skill) {
        cJSON_Delete(root);
        cap_skill_write_error(output, output_size, "out of memory", skill_id_item->valuestring);
        return ESP_ERR_NO_MEM;
    }

    cJSON_Delete(root);
    return cap_skill_build_catalog_result(CAP_SKILL_REGISTER, skill, NULL, output, output_size);
}

static esp_err_t cap_skill_unregister_execute(const char *input_json,
                                              const claw_cap_call_context_t *ctx,
                                              char *output,
                                              size_t output_size)
{
    char skill_path[CAP_SKILL_MAX_PATH_LEN];
    char skill_dir[CAP_SKILL_MAX_PATH_LEN];
    char skill_id[CAP_SKILL_MAX_PATH_LEN];
    char *old_markdown = NULL;
    cJSON *root = NULL;
    cJSON *skill_id_item = NULL;
    claw_skill_catalog_entry_t entry;
    esp_err_t err;

    (void)ctx;

    root = cJSON_Parse(input_json ? input_json : "{}");
    skill_id_item = root ? cJSON_GetObjectItemCaseSensitive(root, "skill_id") : NULL;
    if (!cJSON_IsString(skill_id_item) || !skill_id_item->valuestring || !skill_id_item->valuestring[0]) {
        cJSON_Delete(root);
        cap_skill_write_error(output, output_size, "skill_id is required", NULL);
        return ESP_ERR_INVALID_ARG;
    }
    if (!cap_skill_id_is_valid(skill_id_item->valuestring)) {
        cJSON_Delete(root);
        cap_skill_write_error(output, output_size, "invalid skill_id", skill_id_item->valuestring);
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(skill_id, skill_id_item->valuestring, sizeof(skill_id));
    cJSON_Delete(root);

    err = claw_skill_get_catalog_entry(skill_id, &entry);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "skill %s not found before unregister: %s", skill_id, esp_err_to_name(err));
        cap_skill_write_error(output, output_size, "skill not found", skill_id);
        return err;
    }
    if (entry.manage_mode == CLAW_SKILL_MANAGE_MODE_READONLY) {
        ESP_LOGW(TAG, "reject unregister readonly skill %s", skill_id);
        cap_skill_write_error(output, output_size, "skill is readonly", skill_id);
        return ESP_ERR_INVALID_STATE;
    }
    {
        const char *root_dir = cap_skill_root_dir();
        if (!root_dir) {
            ESP_LOGE(TAG, "skill storage is not initialized for unregister %s", skill_id);
            cap_skill_write_error(output, output_size, "skill storage is not initialized", skill_id);
            return ESP_ERR_INVALID_STATE;
        }
        if (snprintf(skill_path, sizeof(skill_path), "%s/%s", root_dir, entry.file) >= (int)sizeof(skill_path) ||
                snprintf(skill_dir, sizeof(skill_dir), "%s/%s", root_dir, skill_id) >= (int)sizeof(skill_dir)) {
            cap_skill_write_error(output, output_size, "file path is too long", skill_id);
            return ESP_ERR_INVALID_SIZE;
        }
    }
    err = cap_skill_read_file_dup(skill_path, &old_markdown);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to read skill markdown before unregister %s: %s", skill_id, esp_err_to_name(err));
        cap_skill_write_error(output, output_size, "failed to read skill markdown", skill_id);
        return err;
    }
    if (remove(skill_path) != 0) {
        ESP_LOGE(TAG, "failed to delete skill markdown %s", skill_path);
        free(old_markdown);
        cap_skill_write_error(output, output_size, "failed to delete skill markdown", skill_id);
        return ESP_FAIL;
    }

    err = claw_skill_reload_registry();
    if (err != ESP_OK) {
        if (cap_skill_write_file_text(skill_path, old_markdown) == ESP_OK) {
            (void)claw_skill_reload_registry();
        }
        ESP_LOGE(TAG, "failed to reload registry after unregister %s: %s", skill_id, esp_err_to_name(err));
        free(old_markdown);
        cap_skill_write_error(output, output_size, "failed to reload skill registry", skill_id);
        return err;
    }

    free(old_markdown);
    (void)rmdir(skill_dir); /* Best effort; succeeds only when the directory is empty. */
    return cap_skill_build_catalog_result(CAP_SKILL_UNREGISTER, NULL, skill_id, output, output_size);
}

static const claw_cap_descriptor_t s_skill_descriptors[] = {
    {
        .id = "list_skill",
        .name = "list_skill",
        .family = "skill",
        .description = "List all skills discovered from markdown files under the skills root directory.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        /* The skills catalog is already injected into prompt context, so keep this for non-LLM callers only. */
        .cap_flags = 0,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_skill_list_execute,
    },
    {
        .id = "create_skill",
        .name = "create_skill",
        .family = "skill",
        .description = "Create a runtime Skill directly from complete SKILL.md markdown, reload and verify the registry, and optionally activate it for the current session. Use this instead of write_file plus register_skill when creating or repairing a Skill.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{"
        "\"skill_id\":{\"type\":\"string\",\"pattern\":\"^[A-Za-z0-9_-]{1,63}$\"},"
        "\"markdown\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":20480},"
        "\"activate\":{\"type\":\"boolean\"},"
        "\"overwrite\":{\"type\":\"boolean\"}},"
        "\"required\":[\"skill_id\",\"markdown\"]}",
        .execute = cap_skill_create_execute,
    },
    {
        .id = "register_skill",
        .name = "register_skill",
        .family = "skill",
        .description = "Register or refresh an existing source-file skill markdown file and reload the in-memory skill registry. Prefer create_skill when the markdown does not already exist.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"skill_id\":{\"type\":\"string\"},"
        "\"file\":{\"type\":\"string\",\"pattern\":\"^[^/]+/SKILL\\\\.md$\"}},"
        "\"required\":[\"skill_id\",\"file\"]}",
        .execute = cap_skill_register_execute,
    },
    {
        .id = "unregister_skill",
        .name = "unregister_skill",
        .family = "skill",
        .description = "Delete one runtime source-file skill markdown file and reload the in-memory skill registry.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"skill_id\":{\"type\":\"string\"}},\"required\":[\"skill_id\"]}",
        .execute = cap_skill_unregister_execute,
    },
    {
        .id = "activate_skill",
        .name = "activate_skill",
        .family = "skill",
        .description = "Activate a skill from skill_id and return its full Skill markdown document "
                       "inside a <skill_content name=\"skill_id\"> block. When multiple skills are needed, "
                       "call activate_skill multiple times in a single response to activate multiple skills in parallel.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"skill_id\":{\"type\":\"string\"}},\"required\":[\"skill_id\"]}",
        .execute = cap_skill_activate_execute,
    },
};

static const claw_cap_group_t s_skill_group = {
    .group_id = "cap_skill",
    .descriptors = s_skill_descriptors,
    .descriptor_count = sizeof(s_skill_descriptors) / sizeof(s_skill_descriptors[0]),
};

esp_err_t cap_skill_mgr_register_group(const char *skills_root_dir)
{
    if (!skills_root_dir || !skills_root_dir[0]) {
        ESP_LOGE(TAG, "register group: missing skills root dir");
        return ESP_ERR_INVALID_ARG;
    }
    if (snprintf(s_skill_root_dir, sizeof(s_skill_root_dir), "%s", skills_root_dir) >= (int)sizeof(s_skill_root_dir)) {
        s_skill_root_dir[0] = '\0';
        ESP_LOGE(TAG, "register group: skills root dir too long");
        return ESP_ERR_INVALID_SIZE;
    }

    if (claw_cap_group_exists(s_skill_group.group_id)) {
        return ESP_OK;
    }

    return claw_cap_register_group(&s_skill_group);
}