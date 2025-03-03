/*
 * Copyright (C) 1999 Lars Knoll (knoll@kde.org)
 *           (C) 1999 Antti Koivisto (koivisto@kde.org)
 *           (C) 2001 Dirk Mueller (mueller@kde.org)
 * Copyright (C) 2003, 2004, 2005, 2006, 2007, 2008 Apple Inc. All rights
 * reserved.
 * Copyright (C) 2008 Nikolas Zimmermann <zimmermann@kde.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "third_party/blink/renderer/core/script/script_loader.h"

#include "base/feature_list.h"
#include "base/metrics/histogram_functions.h"
#include "services/network/public/mojom/fetch_api.mojom-shared.h"
#include "third_party/blink/public/common/features.h"
#include "third_party/blink/public/common/permissions_policy/permissions_policy.h"
#include "third_party/blink/public/mojom/fetch/fetch_api_request.mojom-blink.h"
#include "third_party/blink/public/mojom/permissions_policy/permissions_policy_feature.mojom-blink.h"
#include "third_party/blink/public/mojom/permissions_policy/policy_disposition.mojom-blink.h"
#include "third_party/blink/renderer/bindings/core/v8/sanitize_script_errors.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_binding_for_core.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/dom/events/event.h"
#include "third_party/blink/renderer/core/dom/scriptable_document_parser.h"
#include "third_party/blink/renderer/core/dom/text.h"
#include "third_party/blink/renderer/core/frame/attribution_src_loader.h"
#include "third_party/blink/renderer/core/frame/csp/content_security_policy.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/html/html_document.h"
#include "third_party/blink/renderer/core/html/parser/html_parser_idioms.h"
#include "third_party/blink/renderer/core/html_names.h"
#include "third_party/blink/renderer/core/inspector/console_message.h"
#include "third_party/blink/renderer/core/loader/fetch_priority_attribute.h"
#include "third_party/blink/renderer/core/loader/modulescript/module_script_creation_params.h"
#include "third_party/blink/renderer/core/loader/modulescript/module_script_fetch_request.h"
#include "third_party/blink/renderer/core/loader/render_blocking_resource_manager.h"
#include "third_party/blink/renderer/core/loader/subresource_integrity_helper.h"
#include "third_party/blink/renderer/core/loader/web_bundle/script_web_bundle.h"
#include "third_party/blink/renderer/core/script/classic_pending_script.h"
#include "third_party/blink/renderer/core/script/classic_script.h"
#include "third_party/blink/renderer/core/script/import_map.h"
#include "third_party/blink/renderer/core/script/js_module_script.h"
#include "third_party/blink/renderer/core/script/modulator.h"
#include "third_party/blink/renderer/core/script/module_pending_script.h"
#include "third_party/blink/renderer/core/script/pending_import_map.h"
#include "third_party/blink/renderer/core/script/script.h"
#include "third_party/blink/renderer/core/script/script_element_base.h"
#include "third_party/blink/renderer/core/script/script_runner.h"
#include "third_party/blink/renderer/core/script_type_names.h"
#include "third_party/blink/renderer/core/speculation_rules/document_speculation_rules.h"
#include "third_party/blink/renderer/core/speculation_rules/speculation_rule_set.h"
#include "third_party/blink/renderer/core/svg_names.h"
#include "third_party/blink/renderer/core/trustedtypes/trusted_types_util.h"
#include "third_party/blink/renderer/platform/bindings/parkable_string.h"
#include "third_party/blink/renderer/platform/heap/garbage_collected.h"
#include "third_party/blink/renderer/platform/instrumentation/use_counter.h"
#include "third_party/blink/renderer/platform/loader/fetch/fetch_client_settings_object_snapshot.h"
#include "third_party/blink/renderer/platform/loader/fetch/fetch_parameters.h"
#include "third_party/blink/renderer/platform/loader/fetch/resource_fetcher.h"
#include "third_party/blink/renderer/platform/loader/subresource_integrity.h"
#include "third_party/blink/renderer/platform/network/mime/mime_type_registry.h"
#include "third_party/blink/renderer/platform/runtime_enabled_features.h"
#include "third_party/blink/renderer/platform/weborigin/security_origin.h"
#include "third_party/blink/renderer/platform/weborigin/security_policy.h"
#include "third_party/blink/renderer/platform/wtf/std_lib_extras.h"
#include "third_party/blink/renderer/platform/wtf/text/string_builder.h"
#include "third_party/blink/renderer/platform/wtf/text/string_hash.h"
#include "third_party/blink/renderer/platform/wtf/text/string_view.h"

namespace blink {

ScriptLoader::ScriptLoader(ScriptElementBase* element,
                           const CreateElementFlags flags)
    : element_(element) {
  // <spec href="https://html.spec.whatwg.org/C/#already-started">... The
  // cloning steps for script elements must set the "already started" flag on
  // the copy if it is set on the element being cloned.</spec>
  //
  // TODO(hiroshige): Cloning is implemented together with
  // {HTML,SVG}ScriptElement::cloneElementWithoutAttributesAndChildren().
  // Clean up these later.
  if (flags.WasAlreadyStarted())
    already_started_ = true;

  if (flags.IsCreatedByParser()) {
    // <spec href="https://html.spec.whatwg.org/C/#parser-inserted">script
    // elements with non-null parser documents are known as
    // "parser-inserted".</spec>
    // For more information on why this is not implemented in terms of a
    // non-null parser document, see the documentation in the header file.
    parser_inserted_ = true;

    // <spec href="https://html.spec.whatwg.org/C/#parser-document">... It is
    // set by the HTML parser and the XML parser on script elements they insert
    // ...</spec>
    parser_document_ = flags.ParserDocument();

    // <spec href="https://html.spec.whatwg.org/C/#non-blocking">... It is unset
    // by the HTML parser and the XML parser on script elements they insert.
    // ...</spec>
    non_blocking_ = false;
  }
}

ScriptLoader::~ScriptLoader() {}

void ScriptLoader::Trace(Visitor* visitor) const {
  visitor->Trace(element_);
  visitor->Trace(parser_document_);
  visitor->Trace(prepared_pending_script_);
  visitor->Trace(resource_keep_alive_);
  visitor->Trace(script_web_bundle_);
  visitor->Trace(speculation_rule_set_);
  ResourceFinishObserver::Trace(visitor);
}

void ScriptLoader::DidNotifySubtreeInsertionsToDocument() {
  if (!parser_inserted_)
    PrepareScript();  // FIXME: Provide a real starting line number here.
}

void ScriptLoader::ChildrenChanged() {
  if (!parser_inserted_ && element_->IsConnected())
    PrepareScript();  // FIXME: Provide a real starting line number here.
}

void ScriptLoader::HandleSourceAttribute(const String& source_url) {
  if (IgnoresLoadRequest() || source_url.IsEmpty())
    return;

  PrepareScript();  // FIXME: Provide a real starting line number here.
}

// <specdef href="https://html.spec.whatwg.org/C/#non-blocking">
void ScriptLoader::HandleAsyncAttribute() {
  // <spec>... In addition, whenever a script element whose "non-blocking" flag
  // is set has an async content attribute added, the element's "non-blocking"
  // flag must be unset.</spec>
  non_blocking_ = false;
  dynamic_async_ = true;
}

void ScriptLoader::Removed() {
  // Release webbundle resources which are associated to this loader explicitly
  // without waiting for blink-GC.
  if (ScriptWebBundle* bundle = std::exchange(script_web_bundle_, nullptr))
    bundle->WillReleaseBundleLoaderAndUnregister();

  if (SpeculationRuleSet* rule_set =
          std::exchange(speculation_rule_set_, nullptr)) {
    // Speculation rules in this script no longer apply.
    // Candidate speculations must be re-evaluated.
    DCHECK_EQ(GetScriptType(), ScriptTypeAtPrepare::kSpeculationRules);
    DocumentSpeculationRules::From(element_->GetDocument())
        .RemoveRuleSet(rule_set);
  }
}

namespace {

// <specdef href="https://html.spec.whatwg.org/C/#prepare-a-script">
bool IsValidClassicScriptTypeAndLanguage(const String& type,
                                         const String& language) {
  if (type.IsNull()) {
    // <spec step="8">the script element has no type attribute but it has a
    // language attribute and that attribute's value is the empty string,
    // or</spec>
    //
    // <spec step="8">the script element has neither a type attribute
    // nor a language attribute, then</spec>
    if (language.IsEmpty())
      return true;

    // <spec step="8">Otherwise, the element has a non-empty language attribute;
    // let the script block's type string for this script element be the
    // concatenation of the string "text/" followed by the value of the language
    // attribute.</spec>
    if (MIMETypeRegistry::IsSupportedJavaScriptMIMEType("text/" + language))
      return true;
  } else if (type.IsEmpty()) {
    // <spec step="8">the script element has a type attribute and its value is
    // the empty string, or</spec>
    return true;
  } else {
    // <spec step="8">Otherwise, if the script element has a type attribute, let
    // the script block's type string for this script element be the value of
    // that attribute with leading and trailing ASCII whitespace
    // stripped.</spec>
    if (MIMETypeRegistry::IsSupportedJavaScriptMIMEType(
            type.StripWhiteSpace())) {
      return true;
    }
  }

  return false;
}

enum class ShouldFireErrorEvent {
  kDoNotFire,
  kShouldFire,
};

bool ShouldForceDeferScript() {
  return base::FeatureList::IsEnabled(features::kForceDeferScriptIntervention);
}

}  // namespace

ScriptLoader::ScriptTypeAtPrepare ScriptLoader::GetScriptTypeAtPrepare(
    const String& type,
    const String& language) {
  if (IsValidClassicScriptTypeAndLanguage(type, language)) {
    // <spec step="8">... If the script block's type string is a JavaScript MIME
    // type essence match, the script's type is "classic". ...</spec>
    return ScriptTypeAtPrepare::kClassic;
  }

  if (EqualIgnoringASCIICase(type, script_type_names::kModule)) {
    // <spec step="8">... If the script block's type string is an ASCII
    // case-insensitive match for the string "module", the script's type is
    // "module". ...</spec>
    return ScriptTypeAtPrepare::kModule;
  }

  if (EqualIgnoringASCIICase(type, script_type_names::kImportmap)) {
    return ScriptTypeAtPrepare::kImportMap;
  }

  if (EqualIgnoringASCIICase(type, script_type_names::kSpeculationrules)) {
    return ScriptTypeAtPrepare::kSpeculationRules;
  }
  if (EqualIgnoringASCIICase(type, script_type_names::kWebbundle)) {
    return ScriptTypeAtPrepare::kWebBundle;
  }

  // <spec step="8">... If neither of the above conditions are true, then
  // return. No script is executed.</spec>
  return ScriptTypeAtPrepare::kInvalid;
}

bool ScriptLoader::BlockForNoModule(ScriptTypeAtPrepare script_type,
                                    bool nomodule) {
  return nomodule && script_type == ScriptTypeAtPrepare::kClassic;
}

// Corresponds to
// https://html.spec.whatwg.org/C/#module-script-credentials-mode
// which is a translation of the CORS settings attribute in the context of
// module scripts. This is used in:
//   - Step 17 of
//     https://html.spec.whatwg.org/C/#prepare-a-script
//   - Step 6 of obtaining a preloaded module script
//     https://html.spec.whatwg.org/C/#link-type-modulepreload.
network::mojom::CredentialsMode ScriptLoader::ModuleScriptCredentialsMode(
    CrossOriginAttributeValue cross_origin) {
  switch (cross_origin) {
    case kCrossOriginAttributeNotSet:
    case kCrossOriginAttributeAnonymous:
      return network::mojom::CredentialsMode::kSameOrigin;
    case kCrossOriginAttributeUseCredentials:
      return network::mojom::CredentialsMode::kInclude;
  }
  NOTREACHED();
  return network::mojom::CredentialsMode::kOmit;
}

// https://github.com/WICG/document-policy/issues/2
bool ShouldBlockSyncScriptForDocumentPolicy(
    const ScriptElementBase* element,
    ScriptLoader::ScriptTypeAtPrepare script_type,
    bool parser_inserted) {
  if (element->GetExecutionContext()->IsFeatureEnabled(
          mojom::blink::DocumentPolicyFeature::kSyncScript)) {
    return false;
  }

  // Non-classic scripts don't block parsing.
  if (script_type == ScriptLoader::ScriptTypeAtPrepare::kModule ||
      script_type == ScriptLoader::ScriptTypeAtPrepare::kImportMap ||
      script_type == ScriptLoader::ScriptTypeAtPrepare::kSpeculationRules ||
      script_type == ScriptLoader::ScriptTypeAtPrepare::kWebBundle ||
      !parser_inserted)
    return false;

  if (!element->HasSourceAttribute())
    return true;
  return !element->DeferAttributeValue() && !element->AsyncAttributeValue();
}

// <specdef href="https://html.spec.whatwg.org/C/#prepare-a-script">
bool ScriptLoader::PrepareScript(const TextPosition& script_start_position) {
  // <spec step="1">If the script element is marked as having "already started",
  // then return. The script is not executed.</spec>
  if (already_started_)
    return false;

  // <spec step="2">If the element has its "parser-inserted" flag set, then set
  // was-parser-inserted to true and unset the element's "parser-inserted" flag.
  // Otherwise, set was-parser-inserted to false.</spec>
  bool was_parser_inserted;
  if (parser_inserted_) {
    was_parser_inserted = true;
    parser_inserted_ = false;
  } else {
    was_parser_inserted = false;
  }

  // <spec step="3">If was-parser-inserted is true and the element does not have
  // an async attribute, then set the element's "non-blocking" flag to
  // true.</spec>
  if (was_parser_inserted && !element_->AsyncAttributeValue())
    non_blocking_ = true;

  // <spec step="4">Let source text be the element's child text content.</spec>
  //
  // Trusted Types additionally requires:
  // https://w3c.github.io/webappsec-trusted-types/dist/spec/#slot-value-verification
  // - Step 4: Execute the Prepare the script URL and text algorithm upon the
  //     script element. If that algorithm threw an error, then return. The
  //     script is not executed.
  // - Step 5: Let source text be the element’s [[ScriptText]] internal slot
  //     value.
  const String source_text = GetScriptText();

  // <spec step="5">If the element has no src attribute, and source text is the
  // empty string, then return. The script is not executed.</spec>
  if (!element_->HasSourceAttribute() && source_text.IsEmpty())
    return false;

  // <spec step="6">If the element is not connected, then return. The script is
  // not executed.</spec>
  if (!element_->IsConnected())
    return false;

  Document& element_document = element_->GetDocument();
  LocalDOMWindow* context_window = element_document.domWindow();

  // <spec step="7">... Determine the script's type as follows: ...</spec>
  script_type_ = GetScriptTypeAtPrepare(element_->TypeAttributeValue(),
                                        element_->LanguageAttributeValue());

  switch (GetScriptType()) {
    case ScriptTypeAtPrepare::kInvalid:
      return false;

    case ScriptTypeAtPrepare::kSpeculationRules:
      if (!RuntimeEnabledFeatures::SpeculationRulesEnabled(context_window))
        return false;
      break;

    case ScriptTypeAtPrepare::kWebBundle:
      if (!RuntimeEnabledFeatures::SubresourceWebBundlesEnabled(
              element_document.GetExecutionContext())) {
        script_type_ = ScriptTypeAtPrepare::kInvalid;
        return false;
      }
      break;

    case ScriptTypeAtPrepare::kClassic:
    case ScriptTypeAtPrepare::kModule:
    case ScriptTypeAtPrepare::kImportMap:
      break;
  }

  // <spec step="8">If was-parser-inserted is true, then flag the element as
  // "parser-inserted" again, and set the element's "non-blocking" flag to
  // false.</spec>
  if (was_parser_inserted) {
    parser_inserted_ = true;
    non_blocking_ = false;
  }

  // <spec step="9">Set the element's "already started" flag.</spec>
  already_started_ = true;

  // <spec step="10">If the element is flagged as "parser-inserted", but the
  // element's node document is not the Document of the parser that created the
  // element, then return.</spec>
  if (parser_inserted_ && parser_document_ != &element_->GetDocument()) {
    return false;
  }

  // <spec step="11">If scripting is disabled for the script element, then
  // return. The script is not executed.</spec>
  //
  // <spec href="https://html.spec.whatwg.org/C/#concept-n-noscript">Scripting
  // is disabled for a node if there is no such browsing context, or if
  // scripting is disabled in that browsing context.</spec>
  if (!context_window)
    return false;
  if (!context_window->CanExecuteScripts(kAboutToExecuteScript))
    return false;

  // <spec step="12">If the script element has a nomodule content attribute and
  // the script's type is "classic", then return. The script is not
  // executed.</spec>
  if (BlockForNoModule(GetScriptType(), element_->NomoduleAttributeValue()))
    return false;

  // TODO(csharrison): This logic only works if the tokenizer/parser was not
  // blocked waiting for scripts when the element was inserted. This usually
  // fails for instance, on second document.write if a script writes twice
  // in a row. To fix this, the parser might have to keep track of raw
  // string position.
  //
  // Also PendingScript's contructor has the same code.
  const bool is_in_document_write = element_document.IsInDocumentWrite();

  // Reset line numbering for nested writes.
  TextPosition position = is_in_document_write ? TextPosition::MinimumPosition()
                                               : script_start_position;

  // <spec step="13">If the script element does not have a src content
  // attribute, and the Should element's inline behavior be blocked by Content
  // Security Policy? algorithm returns "Blocked" when executed upon the script
  // element, "script", and source text, then return. The script is not
  // executed. [CSP]</spec>
  if (!element_->HasSourceAttribute() &&
      !element_->AllowInlineScriptForCSP(element_->GetNonceForElement(),
                                         position.line_, source_text)) {
    return false;
  }

  // 14.
  if (!IsScriptForEventSupported())
    return false;

  if (ShouldBlockSyncScriptForDocumentPolicy(element_.Get(), GetScriptType(),
                                             parser_inserted_)) {
    context_window->ReportDocumentPolicyViolation(
        mojom::blink::DocumentPolicyFeature::kSyncScript,
        mojom::blink::PolicyDisposition::kEnforce,
        "Synchronous script execution is disabled by Document Policy");
    return false;
  }

  // 14. is handled below.

  // <spec step="16">Let classic script CORS setting be the current state of the
  // element's crossorigin content attribute.</spec>
  CrossOriginAttributeValue cross_origin =
      GetCrossOriginAttributeValue(element_->CrossOriginAttributeValue());

  // <spec step="17">Let module script credentials mode be the module script
  // credentials mode for the element's crossorigin content attribute.</spec>
  network::mojom::CredentialsMode credentials_mode =
      ModuleScriptCredentialsMode(cross_origin);

  // <spec step="18">Let cryptographic nonce be the element's
  // [[CryptographicNonce]] internal slot's value.</spec>
  String nonce = element_->GetNonceForElement();

  // <spec step="19">If the script element has an integrity attribute, then let
  // integrity metadata be that attribute's value. Otherwise, let integrity
  // metadata be the empty string.</spec>
  String integrity_attr = element_->IntegrityAttributeValue();
  IntegrityMetadataSet integrity_metadata;
  if (!integrity_attr.IsEmpty()) {
    SubresourceIntegrity::IntegrityFeatures integrity_features =
        SubresourceIntegrityHelper::GetFeatures(
            element_->GetExecutionContext());
    SubresourceIntegrity::ReportInfo report_info;
    SubresourceIntegrity::ParseIntegrityAttribute(
        integrity_attr, integrity_features, integrity_metadata, &report_info);
    SubresourceIntegrityHelper::DoReport(*element_->GetExecutionContext(),
                                         report_info);
  }

  // <spec step="20">Let referrer policy be the current state of the element's
  // referrerpolicy content attribute.</spec>
  String referrerpolicy_attr = element_->ReferrerPolicyAttributeValue();
  network::mojom::ReferrerPolicy referrer_policy =
      network::mojom::ReferrerPolicy::kDefault;
  if (!referrerpolicy_attr.IsEmpty()) {
    SecurityPolicy::ReferrerPolicyFromString(
        referrerpolicy_attr, kDoNotSupportReferrerPolicyLegacyKeywords,
        &referrer_policy);
  }

  // <spec href="https://wicg.github.io/priority-hints/#script" step="8">
  // Let fetchpriority be the current state of the element’s fetchpriority
  // attribute.</spec>
  String fetch_priority_attr = element_->FetchPriorityAttributeValue();
  mojom::blink::FetchPriorityHint fetch_priority_hint =
      GetFetchPriorityAttributeValue(fetch_priority_attr);

  // <spec step="21">Let parser metadata be "parser-inserted" if the script
  // element has been flagged as "parser-inserted", and "not-parser-inserted"
  // otherwise.</spec>
  ParserDisposition parser_state =
      IsParserInserted() ? kParserInserted : kNotParserInserted;

  if (GetScriptType() == ScriptLoader::ScriptTypeAtPrepare::kModule)
    UseCounter::Count(*context_window, WebFeature::kPrepareModuleScript);
  else if (GetScriptType() == ScriptTypeAtPrepare::kSpeculationRules)
    UseCounter::Count(*context_window, WebFeature::kSpeculationRules);

  DCHECK(!prepared_pending_script_);

  bool potentially_render_blocking = element_->IsPotentiallyRenderBlocking();
  RenderBlockingBehavior render_blocking_behavior =
      potentially_render_blocking ? RenderBlockingBehavior::kBlocking
                                  : RenderBlockingBehavior::kNonBlocking;

  // TODO(apaseltiner): Propagate the element instead of passing nullptr.
  const auto attribution_reporting_eligibility =
      element_->HasAttributionsrcAttribute() &&
              CanRegisterAttributionInContext(
                  context_window->GetFrame(), /*element=*/nullptr,
                  /*request_id=*/absl::nullopt,
                  AttributionSrcLoader::RegisterContext::kResource)
          ? ScriptFetchOptions::AttributionReportingEligibility::kEligible
          : ScriptFetchOptions::AttributionReportingEligibility::kIneligible;

  // <spec step="22">Let options be a script fetch options whose cryptographic
  // nonce is cryptographic nonce, integrity metadata is integrity metadata,
  // parser metadata is parser metadata, credentials mode is module script
  // credentials mode, and referrer policy is referrer policy.</spec>
  ScriptFetchOptions options(
      nonce, integrity_metadata, integrity_attr, parser_state, credentials_mode,
      referrer_policy, fetch_priority_hint, render_blocking_behavior,
      RejectCoepUnsafeNone(false), attribution_reporting_eligibility);

  // <spec step="23">Let settings object be the element's node document's
  // relevant settings object.</spec>
  //
  // In some cases (mainly for classic scripts) |element_document| is used as
  // the "settings object", while in other cases (mainly for module scripts)
  // |content_document| is used.
  // TODO(hiroshige): Use a consistent Document everywhere.
  auto* fetch_client_settings_object_fetcher = context_window->Fetcher();

  // https://wicg.github.io/import-maps/#integration-prepare-a-script
  // If the script’s type is "importmap": [spec text]
  if (GetScriptType() == ScriptTypeAtPrepare::kImportMap) {
    Modulator* modulator =
        Modulator::From(ToScriptStateForMainWorld(context_window->GetFrame()));
    auto aquiring_state = modulator->GetAcquiringImportMapsState();
    switch (aquiring_state) {
      case Modulator::AcquiringImportMapsState::kAfterModuleScriptLoad:
      case Modulator::AcquiringImportMapsState::kMultipleImportMaps:
        // 1. If the element’s node document's acquiring import maps is false,
        // then queue a task to fire an event named error at the element, and
        // return. [spec text]
        element_document.AddConsoleMessage(MakeGarbageCollected<ConsoleMessage>(
            mojom::blink::ConsoleMessageSource::kJavaScript,
            mojom::blink::ConsoleMessageLevel::kError,
            aquiring_state ==
                    Modulator::AcquiringImportMapsState::kAfterModuleScriptLoad
                ? "An import map is added after module script load was "
                  "triggered."
                : "Multiple import maps are not yet supported. "
                  "https://crbug.com/927119"));
        element_document.GetTaskRunner(TaskType::kDOMManipulation)
            ->PostTask(FROM_HERE,
                       WTF::Bind(&ScriptElementBase::DispatchErrorEvent,
                                 WrapPersistent(element_.Get())));
        return false;

      case Modulator::AcquiringImportMapsState::kAcquiring:
        // 2. Set the element’s node document's acquiring import maps to false.
        // [spec text]
        modulator->SetAcquiringImportMapsState(
            Modulator::AcquiringImportMapsState::kMultipleImportMaps);

        // 3. Assert: the element’s node document's pending import map script is
        // null. [spec text]
        //
        // TODO(crbug.com/922212): Currently there are no implementation for
        // "pending import map script" as we don't support external import maps.
        break;
    }
  }

  // <spec step="24">If the element has a src content attribute, then:</spec>
  if (element_->HasSourceAttribute()) {
    // <spec step="24.1">Let src be the value of the element's src
    // attribute.</spec>
    String src =
        StripLeadingAndTrailingHTMLSpaces(element_->SourceAttributeValue());

    // <spec step="24.2">If src is the empty string, queue a task to fire an
    // event named error at the element, and return.</spec>
    if (src.IsEmpty()) {
      element_document.GetTaskRunner(TaskType::kDOMManipulation)
          ->PostTask(FROM_HERE,
                     WTF::Bind(&ScriptElementBase::DispatchErrorEvent,
                               WrapPersistent(element_.Get())));
      return false;
    }

    // <spec step="24.3">Set the element's from an external file flag.</spec>
    is_external_script_ = true;

    // <spec step="24.4">Parse src relative to the element's node
    // document.</spec>
    KURL url = element_document.CompleteURL(src);

    // <spec step="24.5">If the previous step failed, queue a task to fire an
    // event named error at the element, and return. Otherwise, let url be the
    // resulting URL record.</spec>
    if (!url.IsValid()) {
      element_document.GetTaskRunner(TaskType::kDOMManipulation)
          ->PostTask(FROM_HERE,
                     WTF::Bind(&ScriptElementBase::DispatchErrorEvent,
                               WrapPersistent(element_.Get())));
      return false;
    }

    // If the element is potentially render-blocking, block rendering on the
    // element.
    if (potentially_render_blocking &&
        element_document.GetRenderBlockingResourceManager()) {
      element_document.GetRenderBlockingResourceManager()->AddPendingScript(
          *element_);
    }

    // <spec step="24.6">Switch on the script's type:</spec>
    switch (GetScriptType()) {
      case ScriptTypeAtPrepare::kInvalid:
        NOTREACHED();
        return false;

      case ScriptTypeAtPrepare::kImportMap:
        // TODO(crbug.com/922212): Implement external import maps.
        element_document.AddConsoleMessage(MakeGarbageCollected<ConsoleMessage>(
            mojom::ConsoleMessageSource::kJavaScript,
            mojom::ConsoleMessageLevel::kError,
            "External import maps are not yet supported."));
        element_document.GetTaskRunner(TaskType::kDOMManipulation)
            ->PostTask(FROM_HERE,
                       WTF::Bind(&ScriptElementBase::DispatchErrorEvent,
                                 WrapPersistent(element_.Get())));
        return false;

      case ScriptTypeAtPrepare::kSpeculationRules:
        // TODO(crbug.com/1182803): Implement external speculation rules.
        element_document.AddConsoleMessage(MakeGarbageCollected<ConsoleMessage>(
            mojom::blink::ConsoleMessageSource::kJavaScript,
            mojom::blink::ConsoleMessageLevel::kError,
            "External speculation rules are not yet supported."));
        return false;

      case ScriptTypeAtPrepare::kWebBundle:
        element_document.AddConsoleMessage(MakeGarbageCollected<ConsoleMessage>(
            mojom::blink::ConsoleMessageSource::kJavaScript,
            mojom::blink::ConsoleMessageLevel::kError,
            "External webbundle is not yet supported."));
        element_document.GetTaskRunner(TaskType::kDOMManipulation)
            ->PostTask(FROM_HERE,
                       WTF::Bind(&ScriptElementBase::DispatchErrorEvent,
                                 WrapPersistent(element_.Get())));
        return false;

      case ScriptTypeAtPrepare::kClassic: {
        // - "classic":

        // <spec step="15">If the script element has a charset attribute, then
        // let encoding be the result of getting an encoding from the value of
        // the charset attribute. If the script element does not have a charset
        // attribute, or if getting an encoding failed, let encoding be the same
        // as the encoding of the script element's node document.</spec>
        //
        // TODO(hiroshige): Should we handle failure in getting an encoding?
        WTF::TextEncoding encoding;
        if (!element_->CharsetAttributeValue().IsEmpty())
          encoding = WTF::TextEncoding(element_->CharsetAttributeValue());
        else
          encoding = element_document.Encoding();

        // <spec step="24.6.A">"classic"
        //
        // Fetch a classic script given url, settings object, options, classic
        // script CORS setting, and encoding.</spec>
        FetchClassicScript(url, element_document, options, cross_origin,
                           encoding);
        break;
      }
      case ScriptTypeAtPrepare::kModule: {
        // - "module":

        // Step 15 is skipped because they are not used in module
        // scripts.

        // <spec step="24.6.B">"module"
        //
        // Fetch an external module script graph given url, settings object, and
        // options.</spec>
        Modulator* modulator = Modulator::From(
            ToScriptStateForMainWorld(context_window->GetFrame()));
        FetchModuleScriptTree(url, fetch_client_settings_object_fetcher,
                              modulator, options);
      }
      // <spec step="24.6">When the chosen algorithm asynchronously completes,
      // set the script's script to the result. At that time, the script is
      // ready.
      // ...</spec>
      //
      // When the script is ready,
      // PendingScriptClient::pendingScriptFinished() is used as the
      // notification, and the action to take when the script is ready is
      // specified later, in
      // - ScriptLoader::PrepareScript(), or
      // - HTMLParserScriptRunner,
      // depending on the conditions in Step 25 of "prepare a script".
      break;
    }
  }

  // <spec step="25">If the element does not have a src content attribute, run
  // these substeps:</spec>
  if (!element_->HasSourceAttribute()) {
    // <spec step="24.1">Let src be the value of the element's src
    // attribute.</spec>
    //
    // This step is done later as ScriptElementBase::ChildTextContent():
    // - in ScriptLoader::PrepareScript() (Step 26, 6th Clause),
    // - in HTMLParserScriptRunner::ProcessScriptElementInternal()
    //   (Duplicated code of Step 26, 6th Clause),
    // - in XMLDocumentParser::EndElementNs() (Step 26, 5th Clause), or
    // - in PendingScript::GetSource() (Indirectly used via
    //   HTMLParserScriptRunner::ProcessScriptElementInternal(),
    //   Step 26, 5th Clause).

    // <spec step="25.1">Let base URL be the script element's node document's
    // document base URL.</spec>
    KURL base_url = element_document.BaseURL();

    // <spec step="25.2">Switch on the script's type:</spec>

    switch (GetScriptType()) {
      case ScriptTypeAtPrepare::kInvalid:
        NOTREACHED();
        return false;

      case ScriptTypeAtPrepare::kImportMap: {
        UseCounter::Count(*context_window, WebFeature::kImportMap);

        // https://wicg.github.io/import-maps/#integration-prepare-a-script
        // 1. Let import map parse result be the result of create an import map
        // parse result, given source text, base URL and settings object. [spec
        // text]
        PendingImportMap* pending_import_map =
            PendingImportMap::CreateInline(*element_, source_text, base_url);

        // Because we currently support inline import maps only, the pending
        // import map is ready immediately and thus we call `register an import
        // map` synchronously here.
        pending_import_map->RegisterImportMap();

        return false;
      }
      case ScriptTypeAtPrepare::kWebBundle: {
        DCHECK(RuntimeEnabledFeatures::SubresourceWebBundlesEnabled(
            context_window));
        DCHECK(!script_web_bundle_);

        absl::variant<ScriptWebBundle*, ScriptWebBundleError>
            script_web_bundle_or_error =
                ScriptWebBundle::CreateOrReuseInline(*element_, source_text);
        if (absl::holds_alternative<ScriptWebBundle*>(
                script_web_bundle_or_error)) {
          script_web_bundle_ =
              absl::get<ScriptWebBundle*>(script_web_bundle_or_error);
          DCHECK(script_web_bundle_);
        }
        if (absl::holds_alternative<ScriptWebBundleError>(
                script_web_bundle_or_error)) {
          ScriptWebBundleError error =
              absl::get<ScriptWebBundleError>(script_web_bundle_or_error);
          // Errors with type kSystemError should fire an error event silently
          // for the user, while the other error types should report an
          // exception.
          if (error.GetType() == ScriptWebBundleError::Type::kSystemError) {
            element_->DispatchErrorEvent();
          } else {
            ScriptState* script_state = ToScriptStateForMainWorld(
                To<LocalDOMWindow>(element_->GetExecutionContext())
                    ->GetFrame());
            if (script_state->ContextIsValid()) {
              ScriptState::Scope scope(script_state);
              V8ScriptRunner::ReportException(script_state->GetIsolate(),
                                              error.ToV8(script_state));
            }
          }
        }
        return false;
      }

      case ScriptTypeAtPrepare::kSpeculationRules: {
        // https://wicg.github.io/nav-speculation/speculation-rules.html
        // Let result be the result of parsing speculation rules given source
        // text and base URL.
        // Set the script’s result to result.
        // If the script’s result is not null, append it to the element’s node
        // document's list of speculation rule sets.
        DCHECK(RuntimeEnabledFeatures::SpeculationRulesEnabled(context_window));
        String parse_error;
        if (auto* rule_set = SpeculationRuleSet::ParseInline(
                source_text, base_url, &parse_error)) {
          speculation_rule_set_ = rule_set;
          DocumentSpeculationRules::From(element_document).AddRuleSet(rule_set);
        }
        if (!parse_error.IsNull()) {
          auto* console_message = MakeGarbageCollected<ConsoleMessage>(
              mojom::ConsoleMessageSource::kOther,
              mojom::ConsoleMessageLevel::kWarning,
              "While parsing speculation rules: " + parse_error);
          console_message->SetNodes(element_document.GetFrame(),
                                    {element_->GetDOMNodeId()});
          element_document.AddConsoleMessage(console_message);
        }
        return false;
      }

        // <spec step="25.2.A">"classic"</spec>
      case ScriptTypeAtPrepare::kClassic: {
        // <spec step="25.2.A.1">Let script be the result of creating a classic
        // script using source text, settings object, base URL, and
        // options.</spec>

        ScriptSourceLocationType script_location_type =
            ScriptSourceLocationType::kInline;
        if (!parser_inserted_) {
          script_location_type =
              ScriptSourceLocationType::kInlineInsideGeneratedElement;
        } else if (is_in_document_write) {
          script_location_type =
              ScriptSourceLocationType::kInlineInsideDocumentWrite;
        }

        prepared_pending_script_ = ClassicPendingScript::CreateInline(
            element_, position, base_url, source_text, script_location_type,
            options);

        // <spec step="25.2.A.2">Set the script's script to script.</spec>
        //
        // <spec step="25.2.A.3">The script is ready.</spec>
        //
        // Implemented by ClassicPendingScript.
        break;
      }

        // <spec step="25.2.B">"module"</spec>
      case ScriptTypeAtPrepare::kModule: {
        // <spec step="25.2.B.1">Fetch an inline module script graph, given
        // source text, base URL, settings object, and options. When this
        // asynchronously completes, set the script's script to the result. At
        // that time, the script is ready.</spec>
        //
        // <specdef label="fetch-an-inline-module-script-graph"
        // href="https://html.spec.whatwg.org/C/#fetch-an-inline-module-script-graph">
        KURL source_url = element_document.Url();
        // Strip any fragment identifiers from the source URL reported to
        // DevTools, so that breakpoints hit reliably for inline module
        // scripts, see crbug.com/1338257 for more details.
        if (source_url.HasFragmentIdentifier())
          source_url.RemoveFragmentIdentifier();
        Modulator* modulator = Modulator::From(
            ToScriptStateForMainWorld(context_window->GetFrame()));

        // <spec label="fetch-an-inline-module-script-graph" step="1">Let script
        // be the result of creating a JavaScript module script using source
        // text, settings object, base URL, and options.</spec>

        ModuleScriptCreationParams params(
            source_url, base_url, ScriptSourceLocationType::kInline,
            ModuleType::kJavaScript, ParkableString(source_text.Impl()),
            nullptr);
        ModuleScript* module_script =
            JSModuleScript::Create(params, modulator, options, position);

        // <spec label="fetch-an-inline-module-script-graph" step="2">If script
        // is null, asynchronously complete this algorithm with null, and abort
        // these steps.</spec>
        if (!module_script)
          return false;

        // <spec label="fetch-an-inline-module-script-graph" step="4">Fetch the
        // descendants of and instantiate script, given settings object, the
        // destination "script", and visited set. When this asynchronously
        // completes with final result, asynchronously complete this algorithm
        // with final result.</spec>
        auto* module_tree_client =
            MakeGarbageCollected<ModulePendingScriptTreeClient>();
        modulator->FetchDescendantsForInlineScript(
            module_script, fetch_client_settings_object_fetcher,
            mojom::blink::RequestContextType::SCRIPT,
            network::mojom::RequestDestination::kScript, module_tree_client);
        prepared_pending_script_ = MakeGarbageCollected<ModulePendingScript>(
            element_, module_tree_client, is_external_script_);
        break;
      }
    }
  }

  DCHECK_NE(GetScriptType(), ScriptLoader::ScriptTypeAtPrepare::kImportMap);

  DCHECK(prepared_pending_script_);

  // <spec step="26">Then, follow the first of the following options that
  // describes the situation:</spec>

  // Three flags are used to instruct the caller of prepareScript() to execute
  // a part of Step 25, when |m_willBeParserExecuted| is true:
  // - |m_willBeParserExecuted|
  // - |m_willExecuteWhenDocumentFinishedParsing|
  // - |m_readyToBeParserExecuted|
  // TODO(hiroshige): Clean up the dependency.

  // <spec step="26.A">If the script's type is "classic", and the element has a
  // src attribute, and the element has a defer attribute, and the element has
  // been flagged as "parser-inserted", and the element does not have an async
  // attribute
  //
  // If the script's type is "module", and the element has been flagged as
  // "parser-inserted", and the element does not have an async attribute
  // ...</spec>
  if ((GetScriptType() == ScriptTypeAtPrepare::kClassic &&
       element_->HasSourceAttribute() && element_->DeferAttributeValue() &&
       parser_inserted_ && !element_->AsyncAttributeValue()) ||
      (GetScriptType() == ScriptTypeAtPrepare::kModule && parser_inserted_ &&
       !element_->AsyncAttributeValue())) {
    // This clause is implemented by the caller-side of prepareScript():
    // - HTMLParserScriptRunner::requestDeferredScript(), and
    // - TODO(hiroshige): Investigate XMLDocumentParser::endElementNs()
    will_execute_when_document_finished_parsing_ = true;
    will_be_parser_executed_ = true;

    return true;
  }

  // Check for external script that should be force deferred.
  if (GetScriptType() == ScriptTypeAtPrepare::kClassic &&
      element_->HasSourceAttribute() && ShouldForceDeferScript() &&
      IsA<HTMLDocument>(context_window->document()) && parser_inserted_ &&
      !element_->AsyncAttributeValue()) {
    // In terms of ScriptLoader flags, force deferred scripts behave like
    // parser-blocking scripts, except that |force_deferred_| is set.
    // The caller of PrepareScript()
    // - Force-defers such scripts if the caller supports force-defer
    //   (i.e., HTMLParserScriptRunner); or
    // - Ignores the |force_deferred_| flag and handles such scripts as
    //   parser-blocking scripts (e.g., XMLParserScriptRunner).
    force_deferred_ = true;
    will_be_parser_executed_ = true;

    return true;
  }

  // <spec step="26.B">If the script's type is "classic", and the element has a
  // src attribute, and the element has been flagged as "parser-inserted", and
  // the element does not have an async attribute ...</spec>
  if (GetScriptType() == ScriptTypeAtPrepare::kClassic &&
      element_->HasSourceAttribute() && parser_inserted_ &&
      !element_->AsyncAttributeValue()) {
    // This clause is implemented by the caller-side of prepareScript():
    // - HTMLParserScriptRunner::requestParsingBlockingScript()
    // - TODO(hiroshige): Investigate XMLDocumentParser::endElementNs()
    will_be_parser_executed_ = true;

    return true;
  }

  // <spec step="26.C">If the script's type is "classic", and the element has a
  // src attribute, and the element does not have an async attribute, and the
  // element does not have the "non-blocking" flag set
  //
  // If the script's type is "module", and the element does not have an async
  // attribute, and the element does not have the "non-blocking" flag set
  // ...</spec>
  if ((GetScriptType() == ScriptTypeAtPrepare::kClassic &&
       element_->HasSourceAttribute() && !element_->AsyncAttributeValue() &&
       !non_blocking_) ||
      (GetScriptType() == ScriptTypeAtPrepare::kModule &&
       !element_->AsyncAttributeValue() && !non_blocking_)) {
    // <spec step="26.C">... Add the element to the end of the list of scripts
    // that will execute in order as soon as possible associated with the node
    // document of the script element at the time the prepare a script algorithm
    // started. ...</spec>
    //
    // TODO(hiroshige): Here the context document is used as "node document"
    // while Step 14 uses |elementDocument| as "node document". Fix this.
    context_window->document()->GetScriptRunner()->QueueScriptForExecution(
        TakePendingScript(ScriptSchedulingType::kInOrder));
    // The part "When the script is ready..." is implemented in
    // ScriptRunner::PendingScriptFinished().
    // TODO(hiroshige): Annotate it.

    if (resource_keep_alive_) {
      resource_keep_alive_->AddFinishObserver(
          this, element_document.GetTaskRunner(TaskType::kNetworking).get());
    }

    return true;
  }

  // <spec step="26.D">If the script's type is "classic", and the element has a
  // src attribute
  //
  // If the script's type is "module" ...</spec>
  if ((GetScriptType() == ScriptTypeAtPrepare::kClassic &&
       element_->HasSourceAttribute()) ||
      GetScriptType() == ScriptTypeAtPrepare::kModule) {
    // <spec step="26.D">... The element must be added to the set of scripts
    // that will execute as soon as possible of the node document of the script
    // element at the time the prepare a script algorithm started. When the
    // script is ready, execute the script block and then remove the element
    // from the set of scripts that will execute as soon as possible.</spec>
    //
    // TODO(hiroshige): Here the context document is used as "node document"
    // while Step 14 uses |elementDocument| as "node document". Fix this.
    context_window->document()->GetScriptRunner()->QueueScriptForExecution(
        TakePendingScript(ScriptSchedulingType::kAsync));
    // The part "When the script is ready..." is implemented in
    // ScriptRunner::PendingScriptFinished().
    // TODO(hiroshige): Annotate it.

    if (resource_keep_alive_) {
      resource_keep_alive_->AddFinishObserver(
          this, element_document.GetTaskRunner(TaskType::kNetworking).get());
    }

    return true;
  }

  // The following clauses are executed only if the script's type is "classic"
  // and the element doesn't have a src attribute.
  DCHECK_EQ(GetScriptType(), ScriptTypeAtPrepare::kClassic);
  DCHECK(!is_external_script_);

  // Check for inline script that should be force deferred.
  if (ShouldForceDeferScript() &&
      IsA<HTMLDocument>(context_window->document()) && parser_inserted_) {
    force_deferred_ = true;
    will_be_parser_executed_ = true;
    return true;
  }

  // <spec step="26.E">If the element does not have a src attribute, and the
  // element has been flagged as "parser-inserted", and either the parser that
  // created the script is an XML parser or it's an HTML parser whose script
  // nesting level is not greater than one, and the Document of the HTML parser
  // or XML parser that created the script element has a style sheet that is
  // blocking scripts ...</spec>
  //
  // <spec step="26.E">... has a style sheet that is blocking scripts ...</spec>
  //
  // is implemented in Document::isScriptExecutionReady().
  // Part of the condition check is done in
  // HTMLParserScriptRunner::processScriptElementInternal().
  // TODO(hiroshige): Clean up the split condition check.
  if (!element_->HasSourceAttribute() && parser_inserted_ &&
      !element_document.IsScriptExecutionReady()) {
    // The former part of this clause is
    // implemented by the caller-side of prepareScript():
    // - HTMLParserScriptRunner::requestParsingBlockingScript()
    // - TODO(hiroshige): Investigate XMLDocumentParser::endElementNs()
    will_be_parser_executed_ = true;
    // <spec step="26.E">... Set the element's "ready to be parser-executed"
    // flag. ...</spec>
    ready_to_be_parser_executed_ = true;

    return true;
  }

  // <spec step="26.F">Otherwise
  //
  // Immediately execute the script block, even if other scripts are already
  // executing.</spec>
  //
  // Note: this block is also duplicated in
  // HTMLParserScriptRunner::processScriptElementInternal().
  // TODO(hiroshige): Merge the duplicated code.
  KURL script_url = (!is_in_document_write && parser_inserted_)
                        ? element_document.Url()
                        : KURL();
  TakePendingScript(ScriptSchedulingType::kImmediate)
      ->ExecuteScriptBlock(script_url);
  return true;
}

// https://html.spec.whatwg.org/C/#fetch-a-classic-script
void ScriptLoader::FetchClassicScript(const KURL& url,
                                      Document& document,
                                      const ScriptFetchOptions& options,
                                      CrossOriginAttributeValue cross_origin,
                                      const WTF::TextEncoding& encoding) {
  FetchParameters::DeferOption defer = FetchParameters::kNoDefer;
  if (!parser_inserted_ || element_->AsyncAttributeValue() ||
      element_->DeferAttributeValue()) {
    defer = FetchParameters::kLazyLoad;
  }
  ClassicPendingScript* pending_script = ClassicPendingScript::Fetch(
      url, document, options, cross_origin, encoding, element_, defer);
  prepared_pending_script_ = pending_script;
  resource_keep_alive_ = pending_script->GetResource();
}

// <specdef href="https://html.spec.whatwg.org/C/#prepare-a-script">
void ScriptLoader::FetchModuleScriptTree(
    const KURL& url,
    ResourceFetcher* fetch_client_settings_object_fetcher,
    Modulator* modulator,
    const ScriptFetchOptions& options) {
  // <spec step="24.6.B">"module"
  //
  // Fetch an external module script graph given url, settings object, and
  // options.</spec>
  auto* module_tree_client =
      MakeGarbageCollected<ModulePendingScriptTreeClient>();
  modulator->FetchTree(url, ModuleType::kJavaScript,
                       fetch_client_settings_object_fetcher,
                       mojom::blink::RequestContextType::SCRIPT,
                       network::mojom::RequestDestination::kScript, options,
                       ModuleScriptCustomFetchType::kNone, module_tree_client);
  prepared_pending_script_ = MakeGarbageCollected<ModulePendingScript>(
      element_, module_tree_client, is_external_script_);
}

PendingScript* ScriptLoader::TakePendingScript(
    ScriptSchedulingType scheduling_type) {
  CHECK(prepared_pending_script_);

  // Record usage histograms per script tag.
  if (element_->GetDocument().Url().ProtocolIsInHTTPFamily()) {
    base::UmaHistogramEnumeration("Blink.Script.SchedulingType",
                                  scheduling_type);
  }

  // Record usage histograms per page.
  switch (scheduling_type) {
    case ScriptSchedulingType::kDefer:
      UseCounter::Count(element_->GetDocument(),
                        WebFeature::kScriptSchedulingType_Defer);
      break;
    case ScriptSchedulingType::kParserBlocking:
      UseCounter::Count(element_->GetDocument(),
                        WebFeature::kScriptSchedulingType_ParserBlocking);
      break;
    case ScriptSchedulingType::kParserBlockingInline:
      UseCounter::Count(element_->GetDocument(),
                        WebFeature::kScriptSchedulingType_ParserBlockingInline);
      break;
    case ScriptSchedulingType::kInOrder:
      UseCounter::Count(element_->GetDocument(),
                        WebFeature::kScriptSchedulingType_InOrder);
      break;
    case ScriptSchedulingType::kAsync:
      UseCounter::Count(element_->GetDocument(),
                        WebFeature::kScriptSchedulingType_Async);
      break;
    default:
      break;
  }

  PendingScript* pending_script = prepared_pending_script_;
  prepared_pending_script_ = nullptr;
  pending_script->SetSchedulingType(scheduling_type);
  return pending_script;
}

void ScriptLoader::NotifyFinished() {
  // Historically we clear |resource_keep_alive_| when the scheduling type is
  // kAsync or kInOrder (crbug.com/778799). But if the script resource was
  // served via signed exchange, the script may not be in the HTTPCache, and
  // therefore will need to be refetched over network if it's evicted from the
  // memory cache. So we keep |resource_keep_alive_| to keep the resource in the
  // memory cache.
  if (resource_keep_alive_ &&
      !resource_keep_alive_->GetResponse().IsSignedExchangeInnerResponse()) {
    resource_keep_alive_ = nullptr;
  }
}

bool ScriptLoader::IgnoresLoadRequest() const {
  return already_started_ || is_external_script_ || parser_inserted_ ||
         !element_->IsConnected();
}

// <specdef href="https://html.spec.whatwg.org/C/#prepare-a-script">
bool ScriptLoader::IsScriptForEventSupported() const {
  // <spec step="14.1">Let for be the value of the for attribute.</spec>
  String event_attribute = element_->EventAttributeValue();
  // <spec step="14.2">Let event be the value of the event attribute.</spec>
  String for_attribute = element_->ForAttributeValue();

  // <spec step="14">If the script element has an event attribute and a for
  // attribute, and the script's type is "classic", then:</spec>
  if (GetScriptType() != ScriptTypeAtPrepare::kClassic ||
      event_attribute.IsNull() || for_attribute.IsNull())
    return true;

  // <spec step="14.3">Strip leading and trailing ASCII whitespace from event
  // and for.</spec>
  for_attribute = for_attribute.StripWhiteSpace();
  // <spec step="14.4">If for is not an ASCII case-insensitive match for the
  // string "window", then return. The script is not executed.</spec>
  if (!EqualIgnoringASCIICase(for_attribute, "window"))
    return false;
  event_attribute = event_attribute.StripWhiteSpace();
  // <spec step="14.5">If event is not an ASCII case-insensitive match for
  // either the string "onload" or the string "onload()", then return. The
  // script is not executed.</spec>
  return EqualIgnoringASCIICase(event_attribute, "onload") ||
         EqualIgnoringASCIICase(event_attribute, "onload()");
}

String ScriptLoader::GetScriptText() const {
  // Step 3 of
  // https://w3c.github.io/webappsec-trusted-types/dist/spec/#abstract-opdef-prepare-the-script-url-and-text
  // called from § 4.1.3.3, step 4 of
  // https://w3c.github.io/webappsec-trusted-types/dist/spec/#slot-value-verification
  // This will return the [[ScriptText]] internal slot value after that step,
  // or a null string if the the Trusted Type algorithm threw an error.
  String child_text_content = element_->ChildTextContent();
  DCHECK(!child_text_content.IsNull());
  String script_text_internal_slot = element_->ScriptTextInternalSlot();
  if (child_text_content == script_text_internal_slot)
    return child_text_content;
  return GetStringForScriptExecution(child_text_content,
                                     element_->GetScriptElementType(),
                                     element_->GetExecutionContext());
}

}  // namespace blink
