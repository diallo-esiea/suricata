/* Copyright (C) 2007-2010 Open Information Security Foundation
 *
 * You can copy, redistribute or modify this Program under the terms of
 * the GNU General Public License version 2 as published by the Free
 * Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/**
 * \file
 *
 * \author Anoop Saldanha <anoopsaldanha@gmail.com>
 *
 * Implements support for ssl_state keyword.
 */

#include "suricata-common.h"
#include "threads.h"
#include "debug.h"
#include "decode.h"

#include "detect.h"
#include "detect-parse.h"

#include "detect-engine.h"
#include "detect-engine-mpm.h"
#include "detect-engine-state.h"

#include "flow.h"
#include "flow-var.h"
#include "flow-util.h"

#include "util-debug.h"
#include "util-unittest.h"
#include "util-unittest-helper.h"

#include "app-layer.h"
#include "app-layer-parser.h"

#include "detect-ssl-state.h"

#include "stream-tcp.h"
#include "app-layer-ssl.h"

#define PARSE_REGEX1 "^\\s*([_a-zA-Z0-9]+)(.*)$"
static pcre *parse_regex1;
static pcre_extra *parse_regex1_study;

#define PARSE_REGEX2 "^(?:\\s*[|]\\s*([_a-zA-Z0-9]+))(.*)$"
static pcre *parse_regex2;
static pcre_extra *parse_regex2_study;

int DetectSslStateMatch(ThreadVars *, DetectEngineThreadCtx *, Flow *,
                        uint8_t, void *, Signature *, SigMatch *);
int DetectSslStateSetup(DetectEngineCtx *, Signature *, char *);
void DetectSslStateRegisterTests(void);
void DetectSslStateFree(void *);

/**
 * \brief Registers the keyword handlers for the "ssl_state" keyword.
 */
void DetectSslStateRegister(void)
{
    sigmatch_table[DETECT_AL_SSL_STATE].name = "ssl_state";
    sigmatch_table[DETECT_AL_SSL_STATE].Match = NULL;
    sigmatch_table[DETECT_AL_SSL_STATE].AppLayerMatch = DetectSslStateMatch;
    sigmatch_table[DETECT_AL_SSL_STATE].alproto = ALPROTO_TLS;
    sigmatch_table[DETECT_AL_SSL_STATE].Setup = DetectSslStateSetup;
    sigmatch_table[DETECT_AL_SSL_STATE].Free  = DetectSslStateFree;
    sigmatch_table[DETECT_AL_SSL_STATE].RegisterTests = DetectSslStateRegisterTests;

    const char *eb;
    int eo;
    int opts = 0;

	SCLogDebug("registering ssl_state rule option");

    /* PARSE_REGEX1 */
    parse_regex1 = pcre_compile(PARSE_REGEX1, opts, &eb, &eo, NULL);
    if (parse_regex1 == NULL) {
        SCLogError(SC_ERR_PCRE_COMPILE, "Compile of \"%s\" failed at offset %" PRId32 ": %s",
                   PARSE_REGEX1, eo, eb);
        goto error;
    }

    parse_regex1_study = pcre_study(parse_regex1, 0, &eb);
    if (eb != NULL) {
        SCLogError(SC_ERR_PCRE_STUDY, "pcre study failed: %s", eb);
        goto error;
    }

    /* PARSE_REGEX2 */
    parse_regex2 = pcre_compile(PARSE_REGEX2, opts, &eb, &eo, NULL);
    if (parse_regex2 == NULL) {
        SCLogError(SC_ERR_PCRE_COMPILE, "Compile of \"%s\" failed at offset %" PRId32 ": %s",
                   PARSE_REGEX2, eo, eb);
        goto error;
    }

    parse_regex2_study = pcre_study(parse_regex2, 0, &eb);
    if (eb != NULL) {
        SCLogError(SC_ERR_PCRE_STUDY, "pcre study failed: %s", eb);
        goto error;
    }

error:
    return;
}

/**
 * \brief App layer match function ssl_state keyword.
 *
 * \param tv      Pointer to threadvars.
 * \param det_ctx Pointer to the thread's detection context.
 * \param f       Pointer to the flow.
 * \param flags   Flags.
 * \param state   App layer state.
 * \param s       Sig we are currently inspecting.
 * \param m       SigMatch we are currently inspecting.
 *
 * \retval 1 Match.
 * \retval 0 No match.
 */
int DetectSslStateMatch(ThreadVars *t, DetectEngineThreadCtx *det_ctx,
                        Flow *f, uint8_t flags, void *alstate, Signature *s,
                        SigMatch *m)
{
    int result = 1;
    DetectSslStateData *ssd = (DetectSslStateData *)m->ctx;
    SSLState *ssl_state = (SSLState *)alstate;
    if (ssl_state == NULL) {
        SCLogDebug("no app state, no match");
        return 0;
    }

    if ((ssd->flags & SSL_AL_FLAG_STATE_CLIENT_HELLO) &&
        !(ssl_state->flags & SSL_AL_FLAG_STATE_CLIENT_HELLO)) {
        result = 0;
        goto end;
    }
    if ((ssd->flags & SSL_AL_FLAG_STATE_SERVER_HELLO) &&
        !(ssl_state->flags & SSL_AL_FLAG_STATE_SERVER_HELLO)) {
        result = 0;
        goto end;
    }
    if ((ssd->flags & SSL_AL_FLAG_STATE_CLIENT_KEYX) &&
        !(ssl_state->flags & SSL_AL_FLAG_STATE_CLIENT_KEYX)) {
        result = 0;
        goto end;
    }
    if ((ssd->flags & SSL_AL_FLAG_STATE_SERVER_KEYX) &&
        !(ssl_state->flags & SSL_AL_FLAG_STATE_SERVER_KEYX)) {
        result = 0;
        goto end;
    }

 end:
    return result;
}

/**
 * \brief Parse the arg supplied with ssl_state and return it in a
 *        DetectSslStateData instance.
 *
 * \param arg Pointer to the string to be parsed.
 *
 * \retval ssd  Pointer to DetectSslStateData on succese.
 * \retval NULL On failure.
 */
DetectSslStateData *DetectSslStateParse(char *arg)
{
#define MAX_SUBSTRINGS 30
    int ret = 0, res = 0;
    int ov1[MAX_SUBSTRINGS];
    int ov2[MAX_SUBSTRINGS];
    const char *str1;
    const char *str2;
    uint32_t flags = 0;
    DetectSslStateData *ssd = NULL;

    ret = pcre_exec(parse_regex1, parse_regex1_study, arg, strlen(arg), 0, 0,
                    ov1, MAX_SUBSTRINGS);
    if (ret < 1) {
        SCLogError(SC_ERR_INVALID_SIGNATURE, "Invalid arg \"%s\" supplied to "
                   "ssl_state keyword.", arg);
        goto error;
    }
    res = pcre_get_substring((char *)arg, ov1, MAX_SUBSTRINGS, 1, &str1);
    if (res < 0) {
        SCLogError(SC_ERR_PCRE_GET_SUBSTRING, "pcre_get_substring failed");
        goto error;
    }

    if (strcmp("client_hello", str1) == 0) {
        flags |= DETECT_SSL_STATE_CLIENT_HELLO;
    } else if (strcmp("server_hello", str1) == 0) {
        flags |= DETECT_SSL_STATE_SERVER_HELLO;
    } else if (strcmp("client_keyx", str1) == 0) {
        flags |= DETECT_SSL_STATE_CLIENT_KEYX;
    } else if (strcmp("server_keyx", str1) == 0) {
        flags |= DETECT_SSL_STATE_SERVER_KEYX;
    } else if (strcmp("unknown", str1) == 0) {
        flags |= DETECT_SSL_STATE_UNKNOWN;
    } else {
        SCLogError(SC_ERR_INVALID_SIGNATURE, "Found invalid option \"%s\" "
                   "in ssl_state keyword.", str1);
        goto error;
    }

    pcre_free_substring(str1);

    res = pcre_get_substring((char *)arg, ov1, MAX_SUBSTRINGS, 2, &str1);
    if (res < 0) {
        SCLogError(SC_ERR_PCRE_GET_SUBSTRING, "pcre_get_substring failed");
        goto error;
    }
    while (res > 0) {
        ret = pcre_exec(parse_regex2, parse_regex2_study, str1, strlen(str1), 0, 0,
                        ov2, MAX_SUBSTRINGS);
        if (ret < 1) {
            SCLogError(SC_ERR_INVALID_SIGNATURE, "Invalid arg \"%s\" supplied to "
                       "ssl_state keyword.", arg);
            goto error;
        }
        res = pcre_get_substring((char *)str1, ov2, MAX_SUBSTRINGS, 1, &str2);
        if (res <= 0) {
            SCLogError(SC_ERR_PCRE_GET_SUBSTRING, "pcre_get_substring failed");
            goto error;
        }
        if (strcmp("client_hello", str2) == 0) {
            flags |= DETECT_SSL_STATE_CLIENT_HELLO;
        } else if (strcmp("server_hello", str2) == 0) {
            flags |= DETECT_SSL_STATE_SERVER_HELLO;
        } else if (strcmp("client_keyx", str2) == 0) {
            flags |= DETECT_SSL_STATE_CLIENT_KEYX;
        } else if (strcmp("server_keyx", str2) == 0) {
            flags |= DETECT_SSL_STATE_SERVER_KEYX;
        } else if (strcmp("unknown", str2) == 0) {
            flags |= DETECT_SSL_STATE_UNKNOWN;
        } else {
            SCLogError(SC_ERR_INVALID_SIGNATURE, "Found invalid option \"%s\" "
                       "in ssl_state keyword.", str2);
            goto error;
        }

        res = pcre_get_substring((char *)str1, ov2, MAX_SUBSTRINGS, 2, &str2);
        if (res < 0) {
            SCLogError(SC_ERR_PCRE_GET_SUBSTRING, "pcre_get_substring failed");
            goto error;
        }

        pcre_free_substring(str1);
        str1 = str2;
    }

    if ( (ssd = SCMalloc(sizeof(DetectSslStateData))) == NULL) {
        goto error;
    }
    ssd->flags = flags;

    return ssd;

error:
    if (ssd != NULL)
        DetectSslStateFree(ssd);
    return NULL;
}

 /**
 * \internal
 * \brief Setup function for ssl_state keyword.
 *
 * \param de_ctx Pointer to the Detection Engine Context.
 * \param s      Pointer to the Current Signature
 * \param arg    String holding the arg.
 *
 * \retval  0 On success.
 * \retval -1 On failure.
 */
int DetectSslStateSetup(DetectEngineCtx *de_ctx, Signature *s, char *arg)
{
    DetectSslStateData *ssd = NULL;
    SigMatch *sm = NULL;

    ssd = DetectSslStateParse(arg);
    if (ssd == NULL)
        goto error;

    sm = SigMatchAlloc();
    if (sm == NULL)
        goto error;

    sm->type = DETECT_AL_SSL_STATE;
    sm->ctx = ssd;

    if (s->alproto != ALPROTO_UNKNOWN && s->alproto != ALPROTO_TLS) {
        SCLogError(SC_ERR_CONFLICTING_RULE_KEYWORDS,
                   "Rule contains conflicting keywords.  Have non-tls alproto "
                   "set for a rule containing \"ssl_state\" keyword");
        goto error;
    }
    s->alproto = ALPROTO_TLS;

    SigMatchAppendSMToList(s, sm, DETECT_SM_LIST_AMATCH);

    return 0;

error:
    if (ssd != NULL)
        DetectSslStateFree(ssd);
    if (sm != NULL)
        SCFree(sm);
    return -1;
}

/**
 * \brief Free memory associated with DetectSslStateData.
 *
 * \param ptr pointer to the data to be freed.
 */
void DetectSslStateFree(void *ptr)
{
    if (ptr != NULL)
        SCFree(ptr);

    return;
}

/************************************Unittests*********************************/

#ifdef UNITTESTS

int DetectSslStateTest01(void)
{
    DetectSslStateData *ssd = DetectSslStateParse("client_hello");
    if (ssd == NULL) {
        printf("ssd == NULL\n");
        return 0;
    }
    if (ssd->flags == DETECT_SSL_STATE_CLIENT_HELLO) {
        SCFree(ssd);
        return 1;
    }

    return 0;
}

int DetectSslStateTest02(void)
{
    DetectSslStateData *ssd = DetectSslStateParse("server_hello | client_hello");
    if (ssd == NULL) {
        printf("ssd == NULL\n");
        return 0;
    }
    if (ssd->flags == (DETECT_SSL_STATE_SERVER_HELLO |
                       DETECT_SSL_STATE_CLIENT_HELLO)) {
        SCFree(ssd);
        return 1;
    }

    return 0;
}

int DetectSslStateTest03(void)
{
    DetectSslStateData *ssd = DetectSslStateParse("server_hello | client_keyx | "
                                                  "client_hello");
    if (ssd == NULL) {
        printf("ssd == NULL\n");
        return 0;
    }
    if (ssd->flags == (DETECT_SSL_STATE_SERVER_HELLO |
                       DETECT_SSL_STATE_CLIENT_KEYX |
                       DETECT_SSL_STATE_CLIENT_HELLO)) {
        SCFree(ssd);
        return 1;
    }

    return 0;
}

int DetectSslStateTest04(void)
{
    DetectSslStateData *ssd = DetectSslStateParse("server_hello | client_keyx | "
                                                  "client_hello | server_keyx | "
                                                  "unknown");
    if (ssd == NULL) {
        printf("ssd == NULL\n");
        return 0;
    }
    if (ssd->flags == (DETECT_SSL_STATE_SERVER_HELLO |
                       DETECT_SSL_STATE_CLIENT_KEYX |
                       DETECT_SSL_STATE_CLIENT_HELLO |
                       DETECT_SSL_STATE_SERVER_KEYX |
                       DETECT_SSL_STATE_UNKNOWN)) {
        SCFree(ssd);
        return 1;
    }

    return 0;
}

int DetectSslStateTest05(void)
{
    DetectSslStateData *ssd = DetectSslStateParse("| server_hello | client_keyx | "
                                                  "client_hello | server_keyx | "
                                                  "unknown");

    if (ssd != NULL) {
        printf("ssd != NULL - failure\n");
        SCFree(ssd);
        return 0;
    }

    return 1;
}

int DetectSslStateTest06(void)
{
    DetectSslStateData *ssd = DetectSslStateParse("server_hello | client_keyx | "
                                                  "client_hello | server_keyx | "
                                                  "unknown | ");
    if (ssd != NULL) {
        printf("ssd != NULL - failure\n");
        SCFree(ssd);
        return 0;
    }

    return 1;
}

/**
 * \test Test a valid dce_iface entry for a bind and bind_ack
 */
static int DetectSslStateTest07(void)
{
    uint8_t chello_buf[] = {
        0x80, 0x67, 0x01, 0x03, 0x00, 0x00, 0x4e, 0x00,
        0x00, 0x00, 0x10, 0x01, 0x00, 0x80, 0x03, 0x00,
        0x80, 0x07, 0x00, 0xc0, 0x06, 0x00, 0x40, 0x02,
        0x00, 0x80, 0x04, 0x00, 0x80, 0x00, 0x00, 0x39,
        0x00, 0x00, 0x38, 0x00, 0x00, 0x35, 0x00, 0x00,
        0x33, 0x00, 0x00, 0x32, 0x00, 0x00, 0x04, 0x00,
        0x00, 0x05, 0x00, 0x00, 0x2f, 0x00, 0x00, 0x16,
        0x00, 0x00, 0x13, 0x00, 0xfe, 0xff, 0x00, 0x00,
        0x0a, 0x00, 0x00, 0x15, 0x00, 0x00, 0x12, 0x00,
        0xfe, 0xfe, 0x00, 0x00, 0x09, 0x00, 0x00, 0x64,
        0x00, 0x00, 0x62, 0x00, 0x00, 0x03, 0x00, 0x00,
        0x06, 0xa8, 0xb8, 0x93, 0xbb, 0x90, 0xe9, 0x2a,
        0xa2, 0x4d, 0x6d, 0xcc, 0x1c, 0xe7, 0x2a, 0x80,
        0x21
    };
    uint32_t chello_buf_len = sizeof(chello_buf);

    uint8_t shello_buf[] = {
        0x16, 0x03, 0x00, 0x00, 0x4a, 0x02,
        0x00, 0x00, 0x46, 0x03, 0x00, 0x44, 0x4c, 0x94,
        0x8f, 0xfe, 0x81, 0xed, 0x93, 0x65, 0x02, 0x88,
        0xa3, 0xf8, 0xeb, 0x63, 0x86, 0x0e, 0x2c, 0xf6,
        0x8d, 0xd0, 0x0f, 0x2c, 0x2a, 0xd6, 0x4f, 0xcd,
        0x2d, 0x3c, 0x16, 0xd7, 0xd6, 0x20, 0xa0, 0xfb,
        0x60, 0x86, 0x3d, 0x1e, 0x76, 0xf3, 0x30, 0xfe,
        0x0b, 0x01, 0xfd, 0x1a, 0x01, 0xed, 0x95, 0xf6,
        0x7b, 0x8e, 0xc0, 0xd4, 0x27, 0xbf, 0xf0, 0x6e,
        0xc7, 0x56, 0xb1, 0x47, 0xce, 0x98, 0x00, 0x35,
        0x00, 0x16, 0x03, 0x00, 0x03, 0x44, 0x0b, 0x00,
        0x03, 0x40, 0x00, 0x03, 0x3d, 0x00, 0x03, 0x3a,
        0x30, 0x82, 0x03, 0x36, 0x30, 0x82, 0x02, 0x9f,
        0xa0, 0x03, 0x02, 0x01, 0x02, 0x02, 0x01, 0x01,
        0x30, 0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86,
        0xf7, 0x0d, 0x01, 0x01, 0x04, 0x05, 0x00, 0x30,
        0x81, 0xa9, 0x31, 0x0b, 0x30, 0x09, 0x06, 0x03,
        0x55, 0x04, 0x06, 0x13, 0x02, 0x58, 0x59, 0x31,
        0x15, 0x30, 0x13, 0x06, 0x03, 0x55, 0x04, 0x08,
        0x13, 0x0c, 0x53, 0x6e, 0x61, 0x6b, 0x65, 0x20,
        0x44, 0x65, 0x73, 0x65, 0x72, 0x74, 0x31, 0x13,
        0x30, 0x11, 0x06, 0x03, 0x55, 0x04, 0x07, 0x13,
        0x0a, 0x53, 0x6e, 0x61, 0x6b, 0x65, 0x20, 0x54,
        0x6f, 0x77, 0x6e, 0x31, 0x17, 0x30, 0x15, 0x06,
        0x03, 0x55, 0x04, 0x0a, 0x13, 0x0e, 0x53, 0x6e,
        0x61, 0x6b, 0x65, 0x20, 0x4f, 0x69, 0x6c, 0x2c,
        0x20, 0x4c, 0x74, 0x64, 0x31, 0x1e, 0x30, 0x1c,
        0x06, 0x03, 0x55, 0x04, 0x0b, 0x13, 0x15, 0x43,
        0x65, 0x72, 0x74, 0x69, 0x66, 0x69, 0x63, 0x61,
        0x74, 0x65, 0x20, 0x41, 0x75, 0x74, 0x68, 0x6f,
        0x72, 0x69, 0x74, 0x79, 0x31, 0x15, 0x30, 0x13,
        0x06, 0x03, 0x55, 0x04, 0x03, 0x13, 0x0c, 0x53,
        0x6e, 0x61, 0x6b, 0x65, 0x20, 0x4f, 0x69, 0x6c,
        0x20, 0x43, 0x41, 0x31, 0x1e, 0x30, 0x1c, 0x06,
        0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01,
        0x09, 0x01, 0x16, 0x0f, 0x63, 0x61, 0x40, 0x73,
        0x6e, 0x61, 0x6b, 0x65, 0x6f, 0x69, 0x6c, 0x2e,
        0x64, 0x6f, 0x6d, 0x30, 0x1e, 0x17, 0x0d, 0x30,
        0x33, 0x30, 0x33, 0x30, 0x35, 0x31, 0x36, 0x34,
        0x37, 0x34, 0x35, 0x5a, 0x17, 0x0d, 0x30, 0x38,
        0x30, 0x33, 0x30, 0x33, 0x31, 0x36, 0x34, 0x37,
        0x34, 0x35, 0x5a, 0x30, 0x81, 0xa7, 0x31, 0x0b,
        0x30, 0x09, 0x06, 0x03, 0x55, 0x04, 0x06, 0x13,
        0x02, 0x58, 0x59, 0x31, 0x15, 0x30, 0x13, 0x06,
        0x03, 0x55, 0x04, 0x08, 0x13, 0x0c, 0x53, 0x6e,
        0x61, 0x6b, 0x65, 0x20, 0x44, 0x65, 0x73, 0x65,
        0x72, 0x74, 0x31, 0x13, 0x30, 0x11, 0x06, 0x03,
        0x55, 0x04, 0x07, 0x13, 0x0a, 0x53, 0x6e, 0x61,
        0x6b, 0x65, 0x20, 0x54, 0x6f, 0x77, 0x6e, 0x31,
        0x17, 0x30, 0x15, 0x06, 0x03, 0x55, 0x04, 0x0a,
        0x13, 0x0e, 0x53, 0x6e, 0x61, 0x6b, 0x65, 0x20,
        0x4f, 0x69, 0x6c, 0x2c, 0x20, 0x4c, 0x74, 0x64,
        0x31, 0x17, 0x30, 0x15, 0x06, 0x03, 0x55, 0x04,
        0x0b, 0x13, 0x0e, 0x57, 0x65, 0x62, 0x73, 0x65,
        0x72, 0x76, 0x65, 0x72, 0x20, 0x54, 0x65, 0x61,
        0x6d, 0x31, 0x19, 0x30, 0x17, 0x06, 0x03, 0x55,
        0x04, 0x03, 0x13, 0x10, 0x77, 0x77, 0x77, 0x2e,
        0x73, 0x6e, 0x61, 0x6b, 0x65, 0x6f, 0x69, 0x6c,
        0x2e, 0x64, 0x6f, 0x6d, 0x31, 0x1f, 0x30, 0x1d,
        0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d,
        0x01, 0x09, 0x01, 0x16, 0x10, 0x77, 0x77, 0x77,
        0x40, 0x73, 0x6e, 0x61, 0x6b, 0x65, 0x6f, 0x69,
        0x6c, 0x2e, 0x64, 0x6f, 0x6d, 0x30, 0x81, 0x9f,
        0x30, 0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86,
        0xf7, 0x0d, 0x01, 0x01, 0x01, 0x05, 0x00, 0x03,
        0x81, 0x8d, 0x00, 0x30, 0x81, 0x89, 0x02, 0x81,
        0x81, 0x00, 0xa4, 0x6e, 0x53, 0x14, 0x0a, 0xde,
        0x2c, 0xe3, 0x60, 0x55, 0x9a, 0xf2, 0x42, 0xa6,
        0xaf, 0x47, 0x12, 0x2f, 0x17, 0xce, 0xfa, 0xba,
        0xdc, 0x4e, 0x63, 0x56, 0x34, 0xb9, 0xba, 0x73,
        0x4b, 0x78, 0x44, 0x3d, 0xc6, 0x6c, 0x69, 0xa4,
        0x25, 0xb3, 0x61, 0x02, 0x9d, 0x09, 0x04, 0x3f,
        0x72, 0x3d, 0xd8, 0x27, 0xd3, 0xb0, 0x5a, 0x45,
        0x77, 0xb7, 0x36, 0xe4, 0x26, 0x23, 0xcc, 0x12,
        0xb8, 0xae, 0xde, 0xa7, 0xb6, 0x3a, 0x82, 0x3c,
        0x7c, 0x24, 0x59, 0x0a, 0xf8, 0x96, 0x43, 0x8b,
        0xa3, 0x29, 0x36, 0x3f, 0x91, 0x7f, 0x5d, 0xc7,
        0x23, 0x94, 0x29, 0x7f, 0x0a, 0xce, 0x0a, 0xbd,
        0x8d, 0x9b, 0x2f, 0x19, 0x17, 0xaa, 0xd5, 0x8e,
        0xec, 0x66, 0xa2, 0x37, 0xeb, 0x3f, 0x57, 0x53,
        0x3c, 0xf2, 0xaa, 0xbb, 0x79, 0x19, 0x4b, 0x90,
        0x7e, 0xa7, 0xa3, 0x99, 0xfe, 0x84, 0x4c, 0x89,
        0xf0, 0x3d, 0x02, 0x03, 0x01, 0x00, 0x01, 0xa3,
        0x6e, 0x30, 0x6c, 0x30, 0x1b, 0x06, 0x03, 0x55,
        0x1d, 0x11, 0x04, 0x14, 0x30, 0x12, 0x81, 0x10,
        0x77, 0x77, 0x77, 0x40, 0x73, 0x6e, 0x61, 0x6b,
        0x65, 0x6f, 0x69, 0x6c, 0x2e, 0x64, 0x6f, 0x6d,
        0x30, 0x3a, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
        0x86, 0xf8, 0x42, 0x01, 0x0d, 0x04, 0x2d, 0x16,
        0x2b, 0x6d, 0x6f, 0x64, 0x5f, 0x73, 0x73, 0x6c,
        0x20, 0x67, 0x65, 0x6e, 0x65, 0x72, 0x61, 0x74,
        0x65, 0x64, 0x20, 0x63, 0x75, 0x73, 0x74, 0x6f,
        0x6d, 0x20, 0x73, 0x65, 0x72, 0x76, 0x65, 0x72,
        0x20, 0x63, 0x65, 0x72, 0x74, 0x69, 0x66, 0x69,
        0x63, 0x61, 0x74, 0x65, 0x30, 0x11, 0x06, 0x09,
        0x60, 0x86, 0x48, 0x01, 0x86, 0xf8, 0x42, 0x01,
        0x01, 0x04, 0x04, 0x03, 0x02, 0x06, 0x40, 0x30,
        0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7,
        0x0d, 0x01, 0x01, 0x04, 0x05, 0x00, 0x03, 0x81,
        0x81, 0x00, 0xae, 0x79, 0x79, 0x22, 0x90, 0x75,
        0xfd, 0xa6, 0xd5, 0xc4, 0xb8, 0xc4, 0x99, 0x4e,
        0x1c, 0x05, 0x7c, 0x91, 0x59, 0xbe, 0x89, 0x0d,
        0x3d, 0xc6, 0x8c, 0xa3, 0xcf, 0xf6, 0xba, 0x23,
        0xdf, 0xb8, 0xae, 0x44, 0x68, 0x8a, 0x8f, 0xb9,
        0x8b, 0xcb, 0x12, 0xda, 0xe6, 0xa2, 0xca, 0xa5,
        0xa6, 0x55, 0xd9, 0xd2, 0xa1, 0xad, 0xba, 0x9b,
        0x2c, 0x44, 0x95, 0x1d, 0x4a, 0x90, 0x59, 0x7f,
        0x83, 0xae, 0x81, 0x5e, 0x3f, 0x92, 0xe0, 0x14,
        0x41, 0x82, 0x4e, 0x7f, 0x53, 0xfd, 0x10, 0x23,
        0xeb, 0x8a, 0xeb, 0xe9, 0x92, 0xea, 0x61, 0xf2,
        0x8e, 0x19, 0xa1, 0xd3, 0x49, 0xc0, 0x84, 0x34,
        0x1e, 0x2e, 0x6e, 0xf6, 0x98, 0xe2, 0x87, 0x53,
        0xd6, 0x55, 0xd9, 0x1a, 0x8a, 0x92, 0x5c, 0xad,
        0xdc, 0x1e, 0x1c, 0x30, 0xa7, 0x65, 0x9d, 0xc2,
        0x4f, 0x60, 0xd2, 0x6f, 0xdb, 0xe0, 0x9f, 0x9e,
        0xbc, 0x41, 0x16, 0x03, 0x00, 0x00, 0x04, 0x0e,
        0x00, 0x00, 0x00
    };
    uint32_t shello_buf_len = sizeof(shello_buf);

    uint8_t client_change_cipher_spec_buf[] = {
        0x16, 0x03, 0x00, 0x00, 0x84, 0x10, 0x00, 0x00,
        0x80, 0x65, 0x51, 0x2d, 0xa6, 0xd4, 0xa7, 0x38,
        0xdf, 0xac, 0x79, 0x1f, 0x0b, 0xd9, 0xb2, 0x61,
        0x7d, 0x73, 0x88, 0x32, 0xd9, 0xf2, 0x62, 0x3a,
        0x8b, 0x11, 0x04, 0x75, 0xca, 0x42, 0xff, 0x4e,
        0xd9, 0xcc, 0xb9, 0xfa, 0x86, 0xf3, 0x16, 0x2f,
        0x09, 0x73, 0x51, 0x66, 0xaa, 0x29, 0xcd, 0x80,
        0x61, 0x0f, 0xe8, 0x13, 0xce, 0x5b, 0x8e, 0x0a,
        0x23, 0xf8, 0x91, 0x5e, 0x5f, 0x54, 0x70, 0x80,
        0x8e, 0x7b, 0x28, 0xef, 0xb6, 0x69, 0xb2, 0x59,
        0x85, 0x74, 0x98, 0xe2, 0x7e, 0xd8, 0xcc, 0x76,
        0x80, 0xe1, 0xb6, 0x45, 0x4d, 0xc7, 0xcd, 0x84,
        0xce, 0xb4, 0x52, 0x79, 0x74, 0xcd, 0xe6, 0xd7,
        0xd1, 0x9c, 0xad, 0xef, 0x63, 0x6c, 0x0f, 0xf7,
        0x05, 0xe4, 0x4d, 0x1a, 0xd3, 0xcb, 0x9c, 0xd2,
        0x51, 0xb5, 0x61, 0xcb, 0xff, 0x7c, 0xee, 0xc7,
        0xbc, 0x5e, 0x15, 0xa3, 0xf2, 0x52, 0x0f, 0xbb,
        0x32, 0x14, 0x03, 0x00, 0x00, 0x01, 0x01, 0x16,
        0x03, 0x00, 0x00, 0x40, 0xa9, 0xd8, 0xd7, 0x35,
        0xbc, 0x39, 0x56, 0x98, 0xad, 0x87, 0x61, 0x2a,
        0xc4, 0x8f, 0xcc, 0x03, 0xcb, 0x93, 0x80, 0x81,
        0xb0, 0x4a, 0xc4, 0xd2, 0x09, 0x71, 0x3e, 0x90,
        0x3c, 0x8d, 0xe0, 0x95, 0x44, 0xfe, 0x56, 0xd1,
        0x7e, 0x88, 0xe2, 0x48, 0xfd, 0x76, 0x70, 0x76,
        0xe2, 0xcd, 0x06, 0xd0, 0xf3, 0x9d, 0x13, 0x79,
        0x67, 0x1e, 0x37, 0xf6, 0x98, 0xbe, 0x59, 0x18,
        0x4c, 0xfc, 0x75, 0x56
    };
    uint32_t client_change_cipher_spec_buf_len =
        sizeof(client_change_cipher_spec_buf);

    uint8_t server_change_cipher_spec_buf[] = {
        0x14, 0x03, 0x00, 0x00, 0x01, 0x01, 0x16, 0x03,
        0x00, 0x00, 0x40, 0xce, 0x7c, 0x92, 0x43, 0x59,
        0xcc, 0x3d, 0x90, 0x91, 0x9c, 0x58, 0xf0, 0x7a,
        0xce, 0xae, 0x0d, 0x08, 0xe0, 0x76, 0xb4, 0x86,
        0xb1, 0x15, 0x5b, 0x32, 0xb8, 0x77, 0x53, 0xe7,
        0xa6, 0xf9, 0xd0, 0x95, 0x5f, 0xaa, 0x07, 0xc3,
        0x96, 0x7c, 0xc9, 0x88, 0xc2, 0x7a, 0x20, 0x89,
        0x4f, 0xeb, 0xeb, 0xb6, 0x19, 0xef, 0xaa, 0x27,
        0x73, 0x9d, 0xa6, 0xb4, 0x9f, 0xeb, 0x34, 0xe2,
        0x4d, 0x9f, 0x6b
    };
    uint32_t server_change_cipher_spec_buf_len =
        sizeof(server_change_cipher_spec_buf);

    uint8_t toserver_app_data_buf[] = {
        0x17, 0x03, 0x00, 0x01, 0xb0, 0x4a, 0xc3, 0x3e,
        0x9d, 0x77, 0x78, 0x01, 0x2c, 0xb4, 0xbc, 0x4c,
        0x9a, 0x84, 0xd7, 0xb9, 0x90, 0x0c, 0x21, 0x10,
        0xf0, 0xfa, 0x00, 0x7c, 0x16, 0xbb, 0x77, 0xfb,
        0x72, 0x42, 0x4f, 0xad, 0x50, 0x4a, 0xd0, 0xaa,
        0x6f, 0xaa, 0x44, 0x6c, 0x62, 0x94, 0x1b, 0xc5,
        0xfe, 0xe9, 0x1c, 0x5e, 0xde, 0x85, 0x0b, 0x0e,
        0x05, 0xe4, 0x18, 0x6e, 0xd2, 0xd3, 0xb5, 0x20,
        0xab, 0x81, 0xfd, 0x18, 0x9a, 0x73, 0xb8, 0xd7,
        0xef, 0xc3, 0xdd, 0x74, 0xd7, 0x9c, 0x1e, 0x6f,
        0x21, 0x6d, 0xf8, 0x24, 0xca, 0x3c, 0x70, 0x78,
        0x36, 0x12, 0x7a, 0x8a, 0x9c, 0xac, 0x4e, 0x1c,
        0xa8, 0xfb, 0x27, 0x30, 0xba, 0x9a, 0xf4, 0x2f,
        0x0a, 0xab, 0x80, 0x6a, 0xa1, 0x60, 0x74, 0xf0,
        0xe3, 0x91, 0x84, 0xe7, 0x90, 0x88, 0xcc, 0xf0,
        0x95, 0x7b, 0x0a, 0x22, 0xf2, 0xf9, 0x27, 0xe0,
        0xdd, 0x38, 0x0c, 0xfd, 0xe9, 0x03, 0x71, 0xdc,
        0x70, 0xa4, 0x6e, 0xdf, 0xe3, 0x72, 0x9e, 0xa1,
        0xf0, 0xc9, 0x00, 0xd6, 0x03, 0x55, 0x6a, 0x67,
        0x5d, 0x9c, 0xb8, 0x75, 0x01, 0xb0, 0x01, 0x9f,
        0xe6, 0xd2, 0x44, 0x18, 0xbc, 0xca, 0x7a, 0x10,
        0x39, 0xa6, 0xcf, 0x15, 0xc7, 0xf5, 0x35, 0xd4,
        0xb3, 0x6d, 0x91, 0x23, 0x84, 0x99, 0xba, 0xb0,
        0x7e, 0xd0, 0xc9, 0x4c, 0xbf, 0x3f, 0x33, 0x68,
        0x37, 0xb7, 0x7d, 0x44, 0xb0, 0x0b, 0x2c, 0x0f,
        0xd0, 0x75, 0xa2, 0x6b, 0x5b, 0xe1, 0x9f, 0xd4,
        0x69, 0x9a, 0x14, 0xc8, 0x29, 0xb7, 0xd9, 0x10,
        0xbb, 0x99, 0x30, 0x9a, 0xfb, 0xcc, 0x13, 0x1f,
        0x76, 0x4e, 0xe6, 0xdf, 0x14, 0xaa, 0xd5, 0x60,
        0xbf, 0x91, 0x49, 0x0d, 0x64, 0x42, 0x29, 0xa8,
        0x64, 0x27, 0xd4, 0x5e, 0x1b, 0x18, 0x03, 0xa8,
        0x73, 0xd6, 0x05, 0x6e, 0xf7, 0x50, 0xb0, 0x09,
        0x6b, 0x69, 0x7a, 0x12, 0x28, 0x58, 0xef, 0x5a,
        0x86, 0x11, 0xde, 0x71, 0x71, 0x9f, 0xca, 0xbd,
        0x79, 0x2a, 0xc2, 0xe5, 0x9b, 0x5e, 0x32, 0xe7,
        0xcb, 0x97, 0x6e, 0xa0, 0xea, 0xa4, 0xa4, 0x6a,
        0x32, 0xf9, 0x37, 0x39, 0xd8, 0x37, 0x6d, 0x63,
        0xf3, 0x08, 0x1c, 0xdd, 0x06, 0xdd, 0x2c, 0x2b,
        0x9f, 0x04, 0x88, 0x5f, 0x36, 0x42, 0xc1, 0xb1,
        0xc7, 0xe8, 0x2d, 0x5d, 0xa4, 0x6c, 0xe5, 0x60,
        0x94, 0xae, 0xd0, 0x90, 0x1e, 0x88, 0xa0, 0x87,
        0x52, 0xfb, 0xed, 0x97, 0xa5, 0x25, 0x5a, 0xb7,
        0x55, 0xc5, 0x13, 0x07, 0x85, 0x27, 0x40, 0xed,
        0xb8, 0xa0, 0x26, 0x13, 0x44, 0x0c, 0xfc, 0xcc,
        0x5a, 0x09, 0xe5, 0x44, 0xb5, 0x63, 0xa1, 0x43,
        0x51, 0x23, 0x4f, 0x17, 0x21, 0x89, 0x2e, 0x58,
        0xfd, 0xf9, 0x63, 0x74, 0x04, 0x70, 0x1e, 0x7d,
        0xd0, 0x66, 0xba, 0x40, 0x5e, 0x45, 0xdc, 0x39,
        0x7c, 0x53, 0x0f, 0xa8, 0x38, 0xb2, 0x13, 0x99,
        0x27, 0xd9, 0x4a, 0x51, 0xe9, 0x9f, 0x2a, 0x92,
        0xbb, 0x9c, 0x90, 0xab, 0xfd, 0xf1, 0xb7, 0x40,
        0x05, 0xa9, 0x7a, 0x20, 0x63, 0x36, 0xc1, 0xef,
        0xb9, 0xad, 0xa2, 0xe0, 0x1d, 0x20, 0x4f, 0xb2,
        0x34, 0xbd, 0xea, 0x07, 0xac, 0x21, 0xce, 0xf6,
        0x8a, 0xa2, 0x9e, 0xcd, 0xfa
    };
    uint32_t toserver_app_data_buf_len = sizeof(toserver_app_data_buf);

    int result = 0;
    Signature *s = NULL;
    ThreadVars th_v;
    Packet *p = NULL;
    Flow f;
    TcpSession ssn;
    DetectEngineThreadCtx *det_ctx = NULL;
    DetectEngineCtx *de_ctx = NULL;
    SSLState *ssl_state = NULL;
    int r = 0;
    AppLayerParserThreadCtx *alp_tctx = AppLayerParserThreadCtxAlloc();

    memset(&th_v, 0, sizeof(th_v));
    memset(&p, 0, sizeof(p));
    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));

    p = UTHBuildPacket(NULL, 0, IPPROTO_TCP);

    FLOW_INITIALIZE(&f);
    f.protoctx = (void *)&ssn;
    f.proto = IPPROTO_TCP;
    p->flow = &f;
    p->flags |= PKT_HAS_FLOW|PKT_STREAM_EST;
    p->flowflags |= FLOW_PKT_TOSERVER;
    p->flowflags |= FLOW_PKT_ESTABLISHED;
    f.alproto = ALPROTO_TLS;

    StreamTcpInitConfig(TRUE);

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;

    s = DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any "
                              "(msg:\"ssl state\"; ssl_state:client_hello; "
                              "sid:1;)");
    if (s == NULL)
        goto end;

    s = DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any "
                              "(msg:\"ssl state\"; "
                              "ssl_state:client_hello | server_hello; "
                              "sid:2;)");
    if (s == NULL)
        goto end;

    s = DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any "
                              "(msg:\"ssl state\"; "
                              "ssl_state:client_hello | server_hello | "
                              "client_keyx; sid:3;)");
    if (s == NULL)
        goto end;

    s = DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any "
                              "(msg:\"ssl state\"; "
                              "ssl_state:client_hello | server_hello | "
                              "client_keyx | server_keyx; sid:4;)");
    if (s == NULL)
        goto end;

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    SCMutexLock(&f.m);
    r = AppLayerParserParse(alp_tctx, &f, ALPROTO_TLS, STREAM_TOSERVER | STREAM_START, chello_buf,
                            chello_buf_len);
    if (r != 0) {
        printf("toserver chunk 1 returned %" PRId32 ", expected 0: ", r);
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);

    ssl_state = f.alstate;
    if (ssl_state == NULL) {
        printf("no ssl state: ");
        goto end;
    }

    /* do detect */
    p->alerts.cnt = 0;
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);

    if (!PacketAlertCheck(p, 1))
        goto end;
    if (PacketAlertCheck(p, 2))
        goto end;
    if (PacketAlertCheck(p, 3))
        goto end;
    if (PacketAlertCheck(p, 4))
        goto end;

    SCMutexLock(&f.m);
    r = AppLayerParserParse(alp_tctx, &f, ALPROTO_TLS, STREAM_TOCLIENT, shello_buf,
                            shello_buf_len);
    if (r != 0) {
        printf("toclient chunk 1 returned %" PRId32 ", expected 0: ", r);
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);

    /* do detect */
    p->alerts.cnt = 0;
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);

    if (PacketAlertCheck(p, 1))
        goto end;
    if (!PacketAlertCheck(p, 2))
        goto end;
    if (PacketAlertCheck(p, 3))
        goto end;
    if (PacketAlertCheck(p, 4))
        goto end;

    SCMutexLock(&f.m);
    r = AppLayerParserParse(alp_tctx, &f, ALPROTO_TLS, STREAM_TOSERVER, client_change_cipher_spec_buf,
                            client_change_cipher_spec_buf_len);
    if (r != 0) {
        printf("toserver chunk 1 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);

    /* do detect */
    p->alerts.cnt = 0;
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);

    if (PacketAlertCheck(p, 1))
        goto end;
    if (PacketAlertCheck(p, 2))
        goto end;
    if (!PacketAlertCheck(p, 3))
        goto end;
    if (PacketAlertCheck(p, 4))
        goto end;

    SCMutexLock(&f.m);
    r = AppLayerParserParse(alp_tctx, &f, ALPROTO_TLS, STREAM_TOCLIENT, server_change_cipher_spec_buf,
                            server_change_cipher_spec_buf_len);
    if (r != 0) {
        printf("toclient chunk 1 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);

    /* do detect */
    p->alerts.cnt = 0;
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);

    if (PacketAlertCheck(p, 1))
        goto end;
    if (PacketAlertCheck(p, 2))
        goto end;
    if (PacketAlertCheck(p, 3))
        goto end;
    if (PacketAlertCheck(p, 4))
        goto end;

    SCMutexLock(&f.m);
    r = AppLayerParserParse(alp_tctx, &f, ALPROTO_TLS, STREAM_TOSERVER, toserver_app_data_buf,
                            toserver_app_data_buf_len);
    if (r != 0) {
        printf("toserver chunk 1 returned %" PRId32 ", expected 0: ", r);
        result = 0;
        SCMutexUnlock(&f.m);
        goto end;
    }
    SCMutexUnlock(&f.m);

    /* do detect */
    p->alerts.cnt = 0;
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);

    if (PacketAlertCheck(p, 1))
        goto end;
    if (PacketAlertCheck(p, 2))
        goto end;
    if (PacketAlertCheck(p, 3))
        goto end;
    if (PacketAlertCheck(p, 4))
        goto end;

    result = 1;

 end:
    if (alp_tctx != NULL)
        AppLayerParserThreadCtxFree(alp_tctx);
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);

    DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
    DetectEngineCtxFree(de_ctx);

    StreamTcpFreeConfig(TRUE);
    FLOW_DESTROY(&f);
    UTHFreePackets(&p, 1);
    return result;
}

#endif /* UNITTESTS */

void DetectSslStateRegisterTests(void)
{
#ifdef UNITTESTS
    UtRegisterTest("DetectSslStateTest01", DetectSslStateTest01, 1);
    UtRegisterTest("DetectSslStateTest02", DetectSslStateTest02, 1);
    UtRegisterTest("DetectSslStateTest03", DetectSslStateTest03, 1);
    UtRegisterTest("DetectSslStateTest04", DetectSslStateTest04, 1);
    UtRegisterTest("DetectSslStateTest05", DetectSslStateTest05, 1);
    UtRegisterTest("DetectSslStateTest06", DetectSslStateTest06, 1);
    UtRegisterTest("DetectSslStateTest07", DetectSslStateTest07, 1);
#endif

    return;
}
