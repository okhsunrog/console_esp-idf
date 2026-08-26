/*
 * SPDX-FileCopyrightText: 2016-2021 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include "esp_err.h"

// Forward declaration. Definition in linenoise/linenoise.h.
typedef struct linenoiseCompletions linenoiseCompletions;

/**
 * @brief Parameters for console initialization
 */
typedef struct {
    size_t max_cmdline_length;  //!< length of command line buffer, in bytes
    size_t max_cmdline_args;    //!< maximum number of command line arguments to parse
    uint32_t heap_alloc_caps;   //!< where to (e.g. MALLOC_CAP_SPIRAM) allocate heap objects such as cmds used by esp_console
    int hint_color;             //!< ASCII color code of hint text
    int hint_bold;              //!< Set to 1 to print hint text in bold
} esp_console_config_t;

/**
 * @brief Default console configuration value
 *
 */
#define ESP_CONSOLE_CONFIG_DEFAULT()           \
    {                                          \
        .max_cmdline_length = 256,             \
        .max_cmdline_args = 32,                \
        .heap_alloc_caps = MALLOC_CAP_DEFAULT, \
        .hint_color = 39,                      \
        .hint_bold = 0                         \
    }

/**
 * @brief initialize console module
 * @param config console configuration
 * @note  Call this once before using other console module features
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_NO_MEM if out of memory
 *      - ESP_ERR_INVALID_STATE if already initialized
 *      - ESP_ERR_INVALID_ARG if the configuration is invalid
 */
esp_err_t esp_console_init(const esp_console_config_t *config);

/**
 * @brief de-initialize console module
 * @note  Call this once when done using console module functions
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if not initialized yet
 */
esp_err_t esp_console_deinit(void);

/**
 * @brief Console command main function
 * @param argc number of arguments
 * @param argv array with argc entries, each pointing to a zero-terminated string argument
 * @return console command return code, 0 indicates "success"
 */
typedef int (*esp_console_cmd_func_t)(int argc, char **argv);

/**
 * @brief Console command description
 */
typedef struct {
    /**
     * Command name. Must not be NULL, must not contain spaces.
     * The pointer must be valid until the call to esp_console_deinit.
     */
    const char *command;
    /**
     * Help text for the command, shown by help command.
     * If set, the pointer must be valid until the call to esp_console_deinit.
     * If not set, the command will not be listed in 'help' output.
     */
    const char *help;
    /**
     * Hint text, usually lists possible arguments.
     * If set to NULL, and 'argtable' field is non-NULL, hint will be generated
     * automatically
     */
    const char *hint;
    /**
     * Pointer to a function which implements the command.
     */
    esp_console_cmd_func_t func;
    /**
     * Array or structure of pointers to arg_xxx structures, may be NULL.
     * Used to generate hint text if 'hint' is set to NULL.
     * Array/structure which this field points to must end with an arg_end.
     * Only used for the duration of esp_console_cmd_register call.
     */
    void *argtable;
} esp_console_cmd_t;

/**
 * @brief Register console command
 * @param cmd pointer to the command description; can point to a temporary value
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_NO_MEM if out of memory
 *      - ESP_ERR_INVALID_ARG if command description includes invalid arguments
 */
esp_err_t esp_console_cmd_register(const esp_console_cmd_t *cmd);

/**
 * @brief Run command line
 * @param cmdline command line (command name followed by a number of arguments)
 * @param[out] cmd_ret return code from the command (set if command was run)
 * @return
 *      - ESP_OK, if command was run
 *      - ESP_ERR_INVALID_ARG, if the command line is empty, or only contained
 *        whitespace
 *      - ESP_ERR_NOT_FOUND, if command with given name wasn't registered
 *      - ESP_ERR_INVALID_STATE, if esp_console_init wasn't called
 */
esp_err_t esp_console_run(const char *cmdline, int *cmd_ret);

/**
 * @brief Split command line into arguments in place
 * @verbatim
 * - This function finds whitespace-separated arguments in the given input line.
 *
 *     'abc def 1 20 .3' -> [ 'abc', 'def', '1', '20', '.3' ]
 *
 * - Argument which include spaces may be surrounded with quotes. In this case
 *   spaces are preserved and quotes are stripped.
 *
 *     'abc "123 456" def' -> [ 'abc', '123 456', 'def' ]
 *
 * - Escape sequences may be used to produce backslash, double quote, and space:
 *
 *     'a\ b\\c\"' -> [ 'a b\c"' ]
 * @endverbatim
 * @note Pointers to at most argv_size - 1 arguments are returned in argv array.
 *       The pointer after the last one (i.e. argv[argc]) is set to NULL.
 *
 * @param line pointer to buffer to parse; it is modified in place
 * @param argv array where the pointers to arguments are written
 * @param argv_size number of elements in argv_array (max. number of arguments)
 * @return number of arguments found (argc)
 */
size_t esp_console_split_argv(char *line, char **argv, size_t argv_size);

/**
 * @brief Callback which provides command completion for linenoise library
 *
 * When using linenoise for line editing, command completion support
 * can be enabled like this:
 *
 *   linenoiseSetCompletionCallback(&esp_console_get_completion);
 *
 * @param buf the string typed by the user
 * @param lc linenoiseCompletions to be filled in
 */
void esp_console_get_completion(const char *buf, linenoiseCompletions *lc);

/**
 * @brief Callback which provides command hints for linenoise library
 *
 * When using linenoise for line editing, hints support can be enabled as
 * follows:
 *
 *   linenoiseSetHintsCallback((linenoiseHintsCallback*) &esp_console_get_hint);
 *
 * The extra cast is needed because linenoiseHintsCallback is defined as
 * returning a char* instead of const char*.
 *
 * @param buf line typed by the user
 * @param[out] color ANSI color code to be used when displaying the hint
 * @param[out] bold set to 1 if hint has to be displayed in bold
 * @return string containing the hint text. This string is persistent and should
 *         not be freed (i.e. linenoiseSetFreeHintsCallback should not be used).
 */
const char *esp_console_get_hint(const char *buf, int *color, int *bold);

/**
 * @brief Register a 'help' command
 *
 * Default 'help' command prints the list of registered commands along with
 * hints and help strings if no additional argument is given. If an additional
 * argument is given, the help command will look for a command with the same
 * name and only print the hints and help strings of that command.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE, if esp_console_init wasn't called
 */
esp_err_t esp_console_register_help_command(void);

#ifdef __cplusplus
}
#endif
