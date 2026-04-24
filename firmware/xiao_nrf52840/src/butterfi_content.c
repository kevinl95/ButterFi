/*
 * Placeholder content handler for the XIAO firmware.
 *
 * The cloud stack currently expects the binary ButterFi request/resend
 * protocol described in docs/shared-protocol.md. The moved XIAO scaffold does
 * not implement that contract yet, so this file intentionally logs the gap
 * instead of pretending the end-to-end runtime path exists.
 */

#include <zephyr/logging/log.h>

#include "butterfi_content.h"

LOG_MODULE_REGISTER(butterfi_content, LOG_LEVEL_INF);

void butterfi_content_handle_msg(const struct sid_msg_desc *desc,
                                 const struct sid_msg *msg)
{
    ARG_UNUSED(desc);
    ARG_UNUSED(msg);

    LOG_WRN("XIAO Sidewalk message received, but the ButterFi chunk/resend runtime path is not implemented yet");
}