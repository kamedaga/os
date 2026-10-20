#include <pacha/abi.h>
#pragma once
#include "pacha/account.h"
/* Load both canonical files before accepting launch registration, so filed is
 * never called back while it is itself waiting for a supervisor launch reply. */
int lprs_accounts_load(pacha_fd_t filed_endpoint, struct pacha_accounts *);
