// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/omnibox/browser/shortcuts_backend.h"

#include <stddef.h>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/bind.h"
#include "base/guid.h"
#include "base/i18n/case_conversion.h"
#include "base/metrics/histogram_macros.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/task/thread_pool.h"
#include "base/threading/thread_task_runner_handle.h"
#include "components/omnibox/browser/autocomplete_input.h"
#include "components/omnibox/browser/autocomplete_match.h"
#include "components/omnibox/browser/autocomplete_match_type.h"
#include "components/omnibox/browser/autocomplete_result.h"
#include "components/omnibox/browser/base_search_provider.h"
#include "components/omnibox/browser/in_memory_url_index_types.h"
#include "components/omnibox/browser/omnibox_field_trial.h"
#include "components/omnibox/browser/shortcuts_database.h"

namespace {

// Takes Match classification vector and removes all matched positions,
// compacting repetitions if necessary.
std::string StripMatchMarkers(const ACMatchClassifications& matches) {
  ACMatchClassifications unmatched;
  for (const auto& match : matches) {
    AutocompleteMatch::AddLastClassificationIfNecessary(
        &unmatched, match.offset, match.style & ~ACMatchClassification::MATCH);
  }
  return AutocompleteMatch::ClassificationsToString(unmatched);
}

// Normally shortcuts have the same match type as the original match they were
// created from, but for certain match types, we should modify the shortcut's
// type slightly to reflect that the origin of the shortcut is historical.
AutocompleteMatch::Type GetTypeForShortcut(AutocompleteMatch::Type type) {
  switch (type) {
    case AutocompleteMatchType::URL_WHAT_YOU_TYPED:
    case AutocompleteMatchType::NAVSUGGEST:
    case AutocompleteMatchType::NAVSUGGEST_PERSONALIZED:
      return AutocompleteMatchType::HISTORY_URL;

    case AutocompleteMatchType::SEARCH_OTHER_ENGINE:
      return type;

    default:
      return AutocompleteMatch::IsSearchType(type)
                 ? AutocompleteMatchType::SEARCH_HISTORY
                 : type;
  }
}

// Get either `description_for_shortcuts` if non-empty or fallback to
// `description`.
const std::u16string& GetDescription(const AutocompleteMatch& match) {
  return match.description_for_shortcuts.empty()
             ? match.description
             : match.description_for_shortcuts;
}

// Get either `description_class_for_shortcuts` if non-empty or fallback to
// `description_class`.
const ACMatchClassifications& GetDescriptionClass(
    const AutocompleteMatch& match) {
  return match.description_class_for_shortcuts.empty()
             ? match.description_class
             : match.description_class_for_shortcuts;
}

// Expand the last word in `text` to a full word in `match`'s description.
// E.g., if `text` is 'Cha Aznav' and the match description is
// 'Charles Aznavour', will return 'Ch Aznavour'.
std::u16string ExpandToFullWord(const std::u16string& text,
                                const AutocompleteMatch& match) {
  DCHECK(!text.empty());

  // Look at the description (i.e. title) only. Contents (i.e. URLs) and
  // destination URLs both contain garble often; e.g.,
  // 'docs.google.com/d/3SyB0Y83dG_WuxX'.
  const auto description_words =
      String16VectorFromString16(GetDescription(match), nullptr);

  // Trim the `text` to:
  // 1) Avoid expanding, e.g., the `text` 'Cha Aznav ' to 'Cha Aznav ur'.
  // 2) Avoid truncating the shortcut e.g., 'Cha Aznavour' to 'Cha ' for the
  //    `text` 'C' when `AddOrUpdateShortcut()` appends 3 chars to `text`.
  // 3) Allow expanding, e.g., the `text` 'Cha ' to 'Charles'.
  // 4) Even when not expanding, autocompleting trailing whitespace looks weird.
  const auto trimmed_text = std::u16string(
      base::TrimWhitespace(text, base::TrimPositions::TRIM_TRAILING));

  // Find the last word in `text` to expand.
  WordStarts text_word_starts;
  const auto text_words =
      String16VectorFromString16(trimmed_text, &text_word_starts);
  // Even though `text` won't be empty, it may contain no words if it consists
  // of only symbols and whitespace. Additionally, even if it does contain
  // words, if it ends with symbols, the last word shouldn't be expanded to
  // avoid expanding, e.g., the text 'Cha*' to 'Cha*rles'.
  if (text_words.empty() ||
      text_word_starts.back() + text_words.back().length() !=
          trimmed_text.length()) {
    return trimmed_text;
  }
  // Lower case `text` for case-insensitive matching with `description_words`.
  const auto text_last_word = base::i18n::ToLower(text_words.back());

  // Prioritize the 1st match that's at least 3 chars long. If none are found,
  // fallback to the 1st match of any length. Don't simply find the 1st match of
  // any length, as that could end up matching 'a', 'at', 'the', etc when a more
  // likely candidate exists. Alternative approaches, e.g., longest match,
  // shortest match, or the match closest to the previous word all have
  // undesirable edge cases. E.g. if using longest match, the `text` 'singer C',
  // with match description 'Singer Charles Aznavour Performs Les Comediens',
  // would expand to 'singer Comédiens'.
  std::u16string best_word;
  // Iterate up to 100 `description_words` for performance.
  for (size_t i = 0;
       i < description_words.size() && i < 100 && best_word.length() < 3u;
       ++i) {
    if (description_words[i].length() < 3u && !best_word.empty())
      continue;
    if (!base::StartsWith(base::i18n::ToLower(description_words[i]),
                          text_last_word, base::CompareCase::SENSITIVE))
      continue;
    best_word = description_words[i];
  }

  // Add on the missing letters of `text_last_word`, rather than replace it with
  // `best_word` to preserve capitalization.
  return best_word.empty()
             ? trimmed_text
             : base::StrCat(
                   {trimmed_text, best_word.substr(text_last_word.length())});
}

}  // namespace

// ShortcutsBackend -----------------------------------------------------------

ShortcutsBackend::ShortcutsBackend(
    TemplateURLService* template_url_service,
    std::unique_ptr<SearchTermsData> search_terms_data,
    history::HistoryService* history_service,
    base::FilePath database_path,
    bool suppress_db)
    : template_url_service_(template_url_service),
      search_terms_data_(std::move(search_terms_data)),
      current_state_(NOT_INITIALIZED),
      main_runner_(base::ThreadTaskRunnerHandle::Get()),
      db_runner_(base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::BEST_EFFORT,
           base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN})),
      no_db_access_(suppress_db) {
  if (!suppress_db)
    db_ = new ShortcutsDatabase(database_path);
  if (history_service)
    history_service_observation_.Observe(history_service);
}

bool ShortcutsBackend::Init() {
  if (current_state_ != NOT_INITIALIZED)
    return false;

  if (no_db_access_) {
    current_state_ = INITIALIZED;
    return true;
  }

  current_state_ = INITIALIZING;
  return db_runner_->PostTask(
      FROM_HERE, base::BindOnce(&ShortcutsBackend::InitInternal, this));
}

bool ShortcutsBackend::DeleteShortcutsWithURL(const GURL& shortcut_url) {
  return initialized() && DeleteShortcutsWithURL(shortcut_url, true);
}

bool ShortcutsBackend::DeleteShortcutsBeginningWithURL(
    const GURL& shortcut_url) {
  return initialized() && DeleteShortcutsWithURL(shortcut_url, false);
}

void ShortcutsBackend::AddObserver(ShortcutsBackendObserver* obs) {
  observer_list_.AddObserver(obs);
}

void ShortcutsBackend::RemoveObserver(ShortcutsBackendObserver* obs) {
  observer_list_.RemoveObserver(obs);
}

void ShortcutsBackend::AddOrUpdateShortcut(const std::u16string& text,
                                           const AutocompleteMatch& match) {
#if DCHECK_IS_ON()
  match.Validate();
#endif  // DCHECK_IS_ON()

  // TODO(manukh): If we decide to launch history cluster suggestions, adding
  //  them to the shortcuts provider would be useful to help users get to
  //  repeat journeys but would require some logic to limit the joint history
  //  cluster provider and shortcuts provider history cluster suggestions to
  //  just 1. Until then, don't add history cluster suggestions to the shortcuts
  //  DB to avoid showing more than 1 history cluster suggestion.
  if (match.type == AutocompleteMatchType::HISTORY_CLUSTER)
    return;

  // Trim `text` since `ExpandToFullWord()` trims the shortcut text; otherwise,
  // inputs with trailing whitespace wouldn't match a shortcut even if the user
  // previously used the input with a trailing whitespace.
  const auto text_trimmed =
      OmniboxFieldTrial::IsShortcutExpandingEnabled()
          ? base::TrimWhitespace(text, base::TrimPositions::TRIM_TRAILING)
          : text;

  // `text` may be empty for pedal and zero suggest navigations. `text_trimmed`
  // can additionally be empty for whitespace-only inputs. It's unlikely users
  // will have a predictable navigation with such inputs, so early exit.
  // Besides, `ShortcutsProvider::Start()` also early exits on empty inputs, so
  // there's no reason to add empty-text shortcuts if they won't be used.
  if (text_trimmed.empty())
    return;

  const std::u16string text_trimmed_lowercase(
      base::i18n::ToLower(text_trimmed));
  const base::Time now(base::Time::Now());

  // Look for an existing shortcut to `match` prefixed by `text`. If there is
  // one, it'll be updated. This avoids creating duplicating equivalent
  // shortcuts (e.g. 'g', 'go', & 'goo') with distributed `number_of_hits`s and
  // outdated `last_access_time`s. There could be multiple relevant shortcuts;
  // e.g., the `text` 'wi' could match both shortcuts 'wiki' and 'wild' to
  // 'wiki.org/wild_west'. We only update the 1st shortcut; this is slightly
  // arbitrary but seems to be fine. Deduping these shortcuts would stop the
  // input 'wil' from finding the 2nd shortcut.
  for (ShortcutMap::const_iterator it(
           shortcuts_map_.lower_bound(text_trimmed_lowercase));
       it != shortcuts_map_.end() &&
       base::StartsWith(it->first, text_trimmed_lowercase,
                        base::CompareCase::SENSITIVE);
       ++it) {
    if (match.destination_url == it->second.match_core.destination_url) {
      // When a user navigates to a shortcut after typing a prefix of the
      // shortcut, the shortcut text is replaced with the shorter user input,
      // plus an additional 3 chars to avoid unstable shortcuts. E.g. if the
      // user creates a shortcut with text 'google.com', then navigates
      // typing 'go', the shortcut text should be updated to 'googl'.
      const auto text_and_3_chars = base::StrCat(
          {text_trimmed, it->second.text.substr(text_trimmed.length(), 3)});
      const auto expanded_text = OmniboxFieldTrial::IsShortcutExpandingEnabled()
                                     ? ExpandToFullWord(text_and_3_chars, match)
                                     : text_and_3_chars;
      UpdateShortcut(ShortcutsDatabase::Shortcut(
          it->second.id, expanded_text,
          MatchToMatchCore(match, template_url_service_,
                           search_terms_data_.get()),
          now, it->second.number_of_hits + 1));
      return;
    }
  }

  // If no shortcuts to `match` prefixed by `text` were found, create one.
  const auto expanded_text = OmniboxFieldTrial::IsShortcutExpandingEnabled()
                                 ? ExpandToFullWord(text, match)
                                 : text;
  AddShortcut(ShortcutsDatabase::Shortcut(
      base::GenerateGUID(), expanded_text,
      MatchToMatchCore(match, template_url_service_, search_terms_data_.get()),
      now, 1));
}

ShortcutsBackend::~ShortcutsBackend() {
  db_runner_->ReleaseSoon(FROM_HERE, std::move(db_));
}

// static
ShortcutsDatabase::Shortcut::MatchCore ShortcutsBackend::MatchToMatchCore(
    const AutocompleteMatch& match,
    TemplateURLService* template_url_service,
    SearchTermsData* search_terms_data) {
  const AutocompleteMatch::Type match_type = GetTypeForShortcut(match.type);

  const AutocompleteMatch* normalized_match = &match;
  AutocompleteMatch temp;

  if (AutocompleteMatch::IsSpecializedSearchType(match.type)) {
    DCHECK(match.search_terms_args);
    temp = BaseSearchProvider::CreateSearchSuggestion(
        match.search_terms_args->search_terms, match_type,
        ui::PageTransitionCoreTypeIs(match.transition,
                                     ui::PAGE_TRANSITION_KEYWORD),
        match.GetTemplateURL(template_url_service, false), *search_terms_data);
    normalized_match = &temp;
  }

  return ShortcutsDatabase::Shortcut::MatchCore(
      normalized_match->fill_into_edit, normalized_match->destination_url,
      normalized_match->document_type, normalized_match->contents,
      StripMatchMarkers(normalized_match->contents_class),
      GetDescription(*normalized_match),
      StripMatchMarkers(GetDescriptionClass(*normalized_match)),
      normalized_match->transition, match_type, normalized_match->keyword);
}

void ShortcutsBackend::ShutdownOnUIThread() {
  history_service_observation_.Reset();
}

void ShortcutsBackend::OnURLsDeleted(
    history::HistoryService* history_service,
    const history::DeletionInfo& deletion_info) {
  if (!initialized())
    return;

  if (deletion_info.IsAllHistory()) {
    DeleteAllShortcuts();
    return;
  }

  ShortcutsDatabase::ShortcutIDs shortcut_ids;
  for (const auto& guid_pair : guid_map_) {
    if (std::find_if(
            deletion_info.deleted_rows().begin(),
            deletion_info.deleted_rows().end(),
            history::URLRow::URLRowHasURL(
                guid_pair.second->second.match_core.destination_url)) !=
        deletion_info.deleted_rows().end()) {
      shortcut_ids.push_back(guid_pair.first);
    }
  }
  DeleteShortcutsWithIDs(shortcut_ids);
}

void ShortcutsBackend::InitInternal() {
  DCHECK(current_state_ == INITIALIZING);
  db_->Init();
  ShortcutsDatabase::GuidToShortcutMap shortcuts;
  db_->LoadShortcuts(&shortcuts);
  temp_shortcuts_map_ = std::make_unique<ShortcutMap>();
  temp_guid_map_ = std::make_unique<GuidMap>();
  for (ShortcutsDatabase::GuidToShortcutMap::const_iterator it(
           shortcuts.begin());
       it != shortcuts.end(); ++it) {
    (*temp_guid_map_)[it->first] = temp_shortcuts_map_->insert(
        std::make_pair(base::i18n::ToLower(it->second.text), it->second));
  }
  main_runner_->PostTask(
      FROM_HERE, base::BindOnce(&ShortcutsBackend::InitCompleted, this));
}

void ShortcutsBackend::InitCompleted() {
  temp_guid_map_->swap(guid_map_);
  temp_shortcuts_map_->swap(shortcuts_map_);
  temp_shortcuts_map_.reset(nullptr);
  temp_guid_map_.reset(nullptr);
  // This histogram is expired but the code was intentionally left behind so
  // it can be easily re-enabled when launching Shortcuts provider on Android
  // or iOS.
  UMA_HISTOGRAM_COUNTS_10000("ShortcutsProvider.DatabaseSize",
                             shortcuts_map_.size());
  current_state_ = INITIALIZED;
  for (ShortcutsBackendObserver& observer : observer_list_)
    observer.OnShortcutsLoaded();
}

bool ShortcutsBackend::AddShortcut(
    const ShortcutsDatabase::Shortcut& shortcut) {
  if (!initialized())
    return false;
  DCHECK(guid_map_.find(shortcut.id) == guid_map_.end());
  guid_map_[shortcut.id] = shortcuts_map_.insert(
      std::make_pair(base::i18n::ToLower(shortcut.text), shortcut));
  for (ShortcutsBackendObserver& observer : observer_list_)
    observer.OnShortcutsChanged();
  return no_db_access_ ||
         db_runner_->PostTask(
             FROM_HERE,
             base::BindOnce(base::IgnoreResult(&ShortcutsDatabase::AddShortcut),
                            db_.get(), shortcut));
}

bool ShortcutsBackend::UpdateShortcut(
    const ShortcutsDatabase::Shortcut& shortcut) {
  if (!initialized())
    return false;
  auto it(guid_map_.find(shortcut.id));
  if (it != guid_map_.end())
    shortcuts_map_.erase(it->second);
  guid_map_[shortcut.id] = shortcuts_map_.insert(
      std::make_pair(base::i18n::ToLower(shortcut.text), shortcut));
  for (ShortcutsBackendObserver& observer : observer_list_)
    observer.OnShortcutsChanged();
  return no_db_access_ ||
         db_runner_->PostTask(
             FROM_HERE, base::BindOnce(base::IgnoreResult(
                                           &ShortcutsDatabase::UpdateShortcut),
                                       db_.get(), shortcut));
}

bool ShortcutsBackend::DeleteShortcutsWithIDs(
    const ShortcutsDatabase::ShortcutIDs& shortcut_ids) {
  if (!initialized())
    return false;
  for (const auto& shortcut_id : shortcut_ids) {
    auto it(guid_map_.find(shortcut_id));
    if (it != guid_map_.end()) {
      shortcuts_map_.erase(it->second);
      guid_map_.erase(it);
    }
  }
  for (ShortcutsBackendObserver& observer : observer_list_)
    observer.OnShortcutsChanged();
  return no_db_access_ ||
         db_runner_->PostTask(
             FROM_HERE,
             base::BindOnce(
                 base::IgnoreResult(&ShortcutsDatabase::DeleteShortcutsWithIDs),
                 db_.get(), shortcut_ids));
}

bool ShortcutsBackend::DeleteShortcutsWithURL(const GURL& url,
                                              bool exact_match) {
  const std::string& url_spec = url.spec();
  ShortcutsDatabase::ShortcutIDs shortcut_ids;
  for (auto it(guid_map_.begin()); it != guid_map_.end();) {
    if (exact_match ? (it->second->second.match_core.destination_url == url)
                    : base::StartsWith(
                          it->second->second.match_core.destination_url.spec(),
                          url_spec, base::CompareCase::SENSITIVE)) {
      shortcut_ids.push_back(it->first);
      shortcuts_map_.erase(it->second);
      guid_map_.erase(it++);
    } else {
      ++it;
    }
  }
  for (ShortcutsBackendObserver& observer : observer_list_)
    observer.OnShortcutsChanged();
  return no_db_access_ ||
         db_runner_->PostTask(
             FROM_HERE,
             base::BindOnce(
                 base::IgnoreResult(&ShortcutsDatabase::DeleteShortcutsWithURL),
                 db_.get(), url_spec));
}

bool ShortcutsBackend::DeleteAllShortcuts() {
  if (!initialized())
    return false;
  shortcuts_map_.clear();
  guid_map_.clear();
  for (ShortcutsBackendObserver& observer : observer_list_)
    observer.OnShortcutsChanged();
  return no_db_access_ ||
         db_runner_->PostTask(
             FROM_HERE,
             base::BindOnce(
                 base::IgnoreResult(&ShortcutsDatabase::DeleteAllShortcuts),
                 db_.get()));
}
