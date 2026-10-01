#pragma once

/// \file dispatch.hpp
/// \brief Provides the public durable-dispatch domain umbrella.

#include "dispatch/journal.hpp"
#include "dispatch/journal_store.hpp"
#include "dispatch/lease.hpp"
#include "dispatch/admission.hpp"
#include "dispatch/file_journal_store.hpp"
#include "dispatch/operation_recovery.hpp"
#include "dispatch/operation_worker.hpp"
