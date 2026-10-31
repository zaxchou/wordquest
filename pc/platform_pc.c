// SPDX-License-Identifier: Apache-2.0

#include <stdio.h>

#include "gsp_sim_bridge.h"
#include "wordquest_ui.h"

static wordquest_ui_t state;

esp_gsp_err_t gsp_bridge_app_init(esp_gsp_handle_t ui)
{
    return wordquest_ui_init(ui, &state);
}

void gsp_bridge_app_deinit(esp_gsp_handle_t ui)
{
    wordquest_ui_deinit(ui, &state);
}
