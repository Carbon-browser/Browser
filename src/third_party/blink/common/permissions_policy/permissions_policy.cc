// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/public/common/permissions_policy/permissions_policy.h"

#include "base/containers/contains.h"
#include "base/memory/ptr_util.h"
#include "base/no_destructor.h"
#include "services/network/public/mojom/web_sandbox_flags.mojom-shared.h"
#include "third_party/blink/public/common/client_hints/client_hints.h"
#include "third_party/blink/public/common/permissions_policy/permissions_policy_features.h"
#include "third_party/blink/public/mojom/permissions_policy/permissions_policy.mojom.h"

namespace blink {
namespace {

// Extracts an Allowlist from a ParsedPermissionsPolicyDeclaration.
PermissionsPolicy::Allowlist AllowlistFromDeclaration(
    const ParsedPermissionsPolicyDeclaration& parsed_declaration,
    const PermissionsPolicyFeatureList& feature_list) {
  auto result = PermissionsPolicy::Allowlist();
  if (parsed_declaration.matches_all_origins)
    result.AddAll();
  if (parsed_declaration.matches_opaque_src)
    result.AddOpaqueSrc();
  for (const auto& value : parsed_declaration.allowed_origins)
    result.Add(value);

  return result;
}

}  // namespace

ParsedPermissionsPolicyDeclaration::ParsedPermissionsPolicyDeclaration() =
    default;

ParsedPermissionsPolicyDeclaration::ParsedPermissionsPolicyDeclaration(
    mojom::PermissionsPolicyFeature feature)
    : feature(feature) {}

ParsedPermissionsPolicyDeclaration::ParsedPermissionsPolicyDeclaration(
    mojom::PermissionsPolicyFeature feature,
    const std::vector<url::Origin>& allowed_origins,
    bool matches_all_origins,
    bool matches_opaque_src)
    : feature(feature),
      allowed_origins(allowed_origins),
      matches_all_origins(matches_all_origins),
      matches_opaque_src(matches_opaque_src) {}

ParsedPermissionsPolicyDeclaration::ParsedPermissionsPolicyDeclaration(
    const ParsedPermissionsPolicyDeclaration& rhs) = default;

ParsedPermissionsPolicyDeclaration&
ParsedPermissionsPolicyDeclaration::operator=(
    const ParsedPermissionsPolicyDeclaration& rhs) = default;

ParsedPermissionsPolicyDeclaration::~ParsedPermissionsPolicyDeclaration() =
    default;

bool operator==(const ParsedPermissionsPolicyDeclaration& lhs,
                const ParsedPermissionsPolicyDeclaration& rhs) {
  return std::tie(lhs.feature, lhs.matches_all_origins, lhs.matches_opaque_src,
                  lhs.allowed_origins) ==
         std::tie(rhs.feature, rhs.matches_all_origins, rhs.matches_opaque_src,
                  rhs.allowed_origins);
}

PermissionsPolicy::Allowlist::Allowlist() = default;

PermissionsPolicy::Allowlist::Allowlist(const Allowlist& rhs) = default;

PermissionsPolicy::Allowlist::~Allowlist() = default;

void PermissionsPolicy::Allowlist::Add(const url::Origin& origin) {
  allowed_origins_.push_back(origin);
}

void PermissionsPolicy::Allowlist::AddAll() {
  matches_all_origins_ = true;
}

void PermissionsPolicy::Allowlist::AddOpaqueSrc() {
  matches_opaque_src_ = true;
}

bool PermissionsPolicy::Allowlist::Contains(const url::Origin& origin) const {
  for (const auto& allowed_origin : allowed_origins_) {
    if (origin == allowed_origin)
      return true;
  }
  if (origin.opaque())
    return matches_opaque_src_;
  return matches_all_origins_;
}

bool PermissionsPolicy::Allowlist::MatchesAll() const {
  return matches_all_origins_;
}

void PermissionsPolicy::Allowlist::RemoveMatchesAll() {
  matches_all_origins_ = false;
}

bool PermissionsPolicy::Allowlist::MatchesOpaqueSrc() const {
  return matches_opaque_src_;
}

// static
std::unique_ptr<PermissionsPolicy> PermissionsPolicy::CreateFromParentPolicy(
    const PermissionsPolicy* parent_policy,
    const ParsedPermissionsPolicy& container_policy,
    const url::Origin& origin) {
  return CreateFromParentPolicy(parent_policy, container_policy, origin,
                                GetPermissionsPolicyFeatureList());
}

// static
std::unique_ptr<PermissionsPolicy> PermissionsPolicy::CopyStateFrom(
    const PermissionsPolicy* source) {
  if (!source)
    return nullptr;

  std::unique_ptr<PermissionsPolicy> new_policy =
      base::WrapUnique(new PermissionsPolicy(
          source->origin_, GetPermissionsPolicyFeatureList()));

  new_policy->inherited_policies_ = source->inherited_policies_;
  new_policy->allowlists_ = source->allowlists_;

  return new_policy;
}

// static
std::unique_ptr<PermissionsPolicy> PermissionsPolicy::CreateFromParsedPolicy(
    const ParsedPermissionsPolicy& parsed_policy,
    const url::Origin& origin) {
  return CreateFromParsedPolicy(parsed_policy, origin,
                                GetPermissionsPolicyFeatureList());
}

// static
std::unique_ptr<PermissionsPolicy> PermissionsPolicy::CreateFromParsedPolicy(
    const ParsedPermissionsPolicy& parsed_policy,
    const url::Origin& origin,
    const PermissionsPolicyFeatureList& features) {
  std::unique_ptr<PermissionsPolicy> new_policy =
      base::WrapUnique(new PermissionsPolicy(origin, features));

  new_policy->SetHeaderPolicy(parsed_policy);
  if (!new_policy->allowlists_.empty()) {
    new_policy->allowlists_set_by_manifest_ = true;
  }

  for (const auto& feature : features) {
    new_policy->inherited_policies_[feature.first] =
        base::Contains(new_policy->allowlists_, feature.first) &&
        new_policy->allowlists_[feature.first].Contains(origin);
  }

  return new_policy;
}

bool PermissionsPolicy::IsFeatureEnabledByInheritedPolicy(
    mojom::PermissionsPolicyFeature feature) const {
  DCHECK(base::Contains(inherited_policies_, feature));
  return inherited_policies_.at(feature);
}

bool PermissionsPolicy::IsFeatureEnabled(
    mojom::PermissionsPolicyFeature feature) const {
  return IsFeatureEnabledForOrigin(feature, origin_);
}

bool PermissionsPolicy::IsFeatureEnabledForOrigin(
    mojom::PermissionsPolicyFeature feature,
    const url::Origin& origin) const {
  DCHECK(base::Contains(feature_list_, feature));
  DCHECK(base::Contains(inherited_policies_, feature));

  auto inherited_value = inherited_policies_.at(feature);
  allowlists_checked_ = true;
  auto allowlist = allowlists_.find(feature);
  if (allowlist != allowlists_.end()) {
    return inherited_value && allowlist->second.Contains(origin);
  }

  // If no "allowlist" is specified, return default feature value.
  const PermissionsPolicyFeatureDefault default_policy =
      feature_list_.at(feature);
  if (default_policy == PermissionsPolicyFeatureDefault::EnableForSelf &&
      !origin_.IsSameOriginWith(origin))
    return false;

  return inherited_value;
}

bool PermissionsPolicy::GetFeatureValueForOrigin(
    mojom::PermissionsPolicyFeature feature,
    const url::Origin& origin) const {
  DCHECK(base::Contains(feature_list_, feature));
  DCHECK(base::Contains(inherited_policies_, feature));

  auto inherited_value = inherited_policies_.at(feature);
  allowlists_checked_ = true;
  auto allowlist = allowlists_.find(feature);
  if (allowlist != allowlists_.end()) {
    return inherited_value && allowlist->second.Contains(origin);
  }

  return inherited_value;
}

const PermissionsPolicy::Allowlist PermissionsPolicy::GetAllowlistForDevTools(
    mojom::PermissionsPolicyFeature feature) const {
  // Return an empty allowlist when disabled through inheritance.
  if (!IsFeatureEnabledByInheritedPolicy(feature))
    return PermissionsPolicy::Allowlist();

  // Return defined policy if exists; otherwise return default policy.
  const auto& maybe_allow_list = GetAllowlistForFeatureIfExists(feature);
  if (maybe_allow_list.has_value())
    return maybe_allow_list.value();

  // Note: |allowlists_| purely comes from HTTP header. If a feature is not
  // declared in HTTP header, all origins are implicitly allowed.
  PermissionsPolicy::Allowlist default_allowlist;
  default_allowlist.AddAll();

  return default_allowlist;
}

// TODO(crbug.com/937131): Use |PermissionsPolicy::GetAllowlistForDevTools|
// to replace this method. This method uses legacy |default_allowlist|
// calculation method.
const PermissionsPolicy::Allowlist PermissionsPolicy::GetAllowlistForFeature(
    mojom::PermissionsPolicyFeature feature) const {
  DCHECK(base::Contains(feature_list_, feature));
  // Return an empty allowlist when disabled through inheritance.
  if (!IsFeatureEnabledByInheritedPolicy(feature))
    return PermissionsPolicy::Allowlist();

  // Return defined policy if exists; otherwise return default policy.
  const auto& maybe_allow_list = GetAllowlistForFeatureIfExists(feature);
  if (maybe_allow_list.has_value())
    return maybe_allow_list.value();

  const PermissionsPolicyFeatureDefault default_policy =
      feature_list_.at(feature);
  PermissionsPolicy::Allowlist default_allowlist;

  if (default_policy == PermissionsPolicyFeatureDefault::EnableForAll) {
    default_allowlist.AddAll();
  } else if (default_policy == PermissionsPolicyFeatureDefault::EnableForSelf) {
    default_allowlist.Add(origin_);
  }

  return default_allowlist;
}

absl::optional<const PermissionsPolicy::Allowlist>
PermissionsPolicy::GetAllowlistForFeatureIfExists(
    mojom::PermissionsPolicyFeature feature) const {
  // Return an empty allowlist when disabled through inheritance.
  if (!IsFeatureEnabledByInheritedPolicy(feature))
    return absl::nullopt;

  // Only return allowlist if actually in `allowlists_`.
  allowlists_checked_ = true;
  auto allowlist = allowlists_.find(feature);
  if (allowlist != allowlists_.end())
    return allowlist->second;
  return absl::nullopt;
}

void PermissionsPolicy::SetHeaderPolicy(
    const ParsedPermissionsPolicy& parsed_header) {
  if (allowlists_set_by_manifest_)
    return;
  DCHECK(allowlists_.empty() && !allowlists_checked_);
  for (const ParsedPermissionsPolicyDeclaration& parsed_declaration :
       parsed_header) {
    mojom::PermissionsPolicyFeature feature = parsed_declaration.feature;
    DCHECK(feature != mojom::PermissionsPolicyFeature::kNotFound);
    allowlists_.emplace(
        feature, AllowlistFromDeclaration(parsed_declaration, feature_list_));
  }
}

void PermissionsPolicy::SetHeaderPolicyForIsolatedApp(
    const ParsedPermissionsPolicy& parsed_header) {
  DCHECK(!allowlists_checked_);
  for (const ParsedPermissionsPolicyDeclaration& parsed_declaration :
       parsed_header) {
    mojom::PermissionsPolicyFeature feature = parsed_declaration.feature;
    DCHECK(feature != mojom::PermissionsPolicyFeature::kNotFound);
    const auto header_allowlist =
        AllowlistFromDeclaration(parsed_declaration, feature_list_);
    auto& isolated_app_allowlist = allowlists_.at(feature);

    // If the header does not specify further restrictions we do not need to
    // modify the policy.
    if (header_allowlist.MatchesAll())
      continue;

    const auto header_allowed_origins = header_allowlist.AllowedOrigins();
    // If the manifest allows all origins access to this feature, use the more
    // restrictive header policy.
    if (isolated_app_allowlist.MatchesAll()) {
      // TODO(crbug.com/1336275): Refactor to use Allowlist::clone() after
      // clone() is implemented.
      isolated_app_allowlist.SetAllowedOrigins(header_allowed_origins);
      isolated_app_allowlist.RemoveMatchesAll();
      continue;
    }

    // Otherwise, we use the intersection of origins in the manifest and the
    // header.
    auto manifest_allowed_origins = isolated_app_allowlist.AllowedOrigins();
    std::vector<url::Origin> final_allowed_origins;
    for (const auto& origin : manifest_allowed_origins) {
      if (base::Contains(header_allowed_origins, origin)) {
        final_allowed_origins.push_back(origin);
      }
    }
    isolated_app_allowlist.SetAllowedOrigins(final_allowed_origins);
  }
}

void PermissionsPolicy::OverwriteHeaderPolicyForClientHints(
    const ParsedPermissionsPolicy& parsed_header) {
  DCHECK(!allowlists_checked_);
  for (const ParsedPermissionsPolicyDeclaration& parsed_declaration :
       parsed_header) {
    mojom::PermissionsPolicyFeature feature = parsed_declaration.feature;
    DCHECK(GetPolicyFeatureToClientHintMap().contains(feature));
    allowlists_[feature] =
        AllowlistFromDeclaration(parsed_declaration, feature_list_);
  }
}

PermissionsPolicyFeatureState PermissionsPolicy::GetFeatureState() const {
  PermissionsPolicyFeatureState feature_state;
  for (const auto& pair : GetPermissionsPolicyFeatureList())
    feature_state[pair.first] = GetFeatureValueForOrigin(pair.first, origin_);
  return feature_state;
}

PermissionsPolicy::PermissionsPolicy(
    url::Origin origin,
    const PermissionsPolicyFeatureList& feature_list)
    : origin_(std::move(origin)),
      allowlists_checked_(false),
      feature_list_(feature_list) {}

PermissionsPolicy::~PermissionsPolicy() = default;

// static
std::unique_ptr<PermissionsPolicy> PermissionsPolicy::CreateForFencedFrame(
    const url::Origin& origin) {
  return CreateForFencedFrame(origin, GetPermissionsPolicyFeatureList());
}

std::unique_ptr<PermissionsPolicy> PermissionsPolicy::CreateForFencedFrame(
    const url::Origin& origin,
    const PermissionsPolicyFeatureList& features) {
  std::unique_ptr<PermissionsPolicy> new_policy =
      base::WrapUnique(new PermissionsPolicy(origin, features));
  for (const auto& feature : features) {
    new_policy->inherited_policies_[feature.first] = false;
  }
  return new_policy;
}

// static
std::unique_ptr<PermissionsPolicy> PermissionsPolicy::CreateFromParentPolicy(
    const PermissionsPolicy* parent_policy,
    const ParsedPermissionsPolicy& container_policy,
    const url::Origin& origin,
    const PermissionsPolicyFeatureList& features) {
  // If there is a non-empty container policy, then there must also be a parent
  // policy.
  DCHECK(parent_policy || container_policy.empty());

  std::unique_ptr<PermissionsPolicy> new_policy =
      base::WrapUnique(new PermissionsPolicy(origin, features));
  for (const auto& feature : features) {
    new_policy->inherited_policies_[feature.first] =
        new_policy->InheritedValueForFeature(parent_policy, feature,
                                             container_policy);
  }
  return new_policy;
}

// Implements Permissions Policy 9.7: Define an inherited policy for feature in
// browsing context and 9.8: Define an inherited policy for feature in container
// at origin.
bool PermissionsPolicy::InheritedValueForFeature(
    const PermissionsPolicy* parent_policy,
    std::pair<mojom::PermissionsPolicyFeature, PermissionsPolicyFeatureDefault>
        feature,
    const ParsedPermissionsPolicy& container_policy) const {
  // 9.7 2: Otherwise [If context is not a nested browsing context,] return
  // "Enabled".
  if (!parent_policy)
    return true;

  // 9.8 2: If policy’s inherited policy for feature is "Disabled", return
  // "Disabled".
  if (!parent_policy->GetFeatureValueForOrigin(feature.first,
                                               parent_policy->origin_))
    return false;

  // 9.8 3: If feature is present in policy’s declared policy, and the allowlist
  // for feature in policy’s declared policy does not match origin, then return
  // "Disabled".
  if (!parent_policy->GetFeatureValueForOrigin(feature.first, origin_))
    return false;

  for (const auto& decl : container_policy) {
    if (decl.feature == feature.first) {
      // 9.8 5.1: If the allowlist for feature in container policy matches
      // origin, return "Enabled".
      // 9.8 5.2: Otherwise return "Disabled".
      return AllowlistFromDeclaration(decl, feature_list_).Contains(origin_);
    }
  }
  // 9.8 6: If feature’s default allowlist is *, return "Enabled".
  if (feature.second == PermissionsPolicyFeatureDefault::EnableForAll)
    return true;

  // 9.8 7: If feature’s default allowlist is 'self', and origin is same origin
  // with container’s node document’s origin, return "Enabled".
  // 9.8 8: Otherwise return "Disabled".
  return origin_.IsSameOriginWith(parent_policy->origin_);
}

const PermissionsPolicyFeatureList& PermissionsPolicy::GetFeatureList() const {
  return feature_list_;
}

}  // namespace blink
