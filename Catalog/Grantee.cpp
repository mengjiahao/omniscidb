/*
 * Copyright 2017 MapD Technologies, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "Grantee.h"

#include <stack>

#include "Shared/misc.h"

using std::runtime_error;
using std::string;

Grantee::Grantee(const std::string& name) : name_(name) {}

Grantee::~Grantee() {
  // 解绑role与grantee父子关系.
  for (auto role : roles_) {
    role->removeGrantee(this);
  }
  effectivePrivileges_.clear();
  directPrivileges_.clear();
  roles_.clear();
}

/**
 * @brief 递归获取树中所有roles。
 * 
 */
std::vector<std::string> Grantee::getRoles(bool only_direct) const {
  std::set<std::string> roles;  // Sorted for human readers.
  std::stack<const Grantee*> g;
  g.push(this);
  while (!g.empty()) {
    auto r = g.top();
    g.pop();
    for (auto direct_role : r->roles_) {
      g.push(direct_role);
      roles.insert(direct_role->getName());
    }
    if (only_direct) {
      break;
    }
  }
  return std::vector(roles.begin(), roles.end());
}

bool Grantee::hasRole(Role* role, bool only_direct) const {
  if (only_direct) {
    return roles_.find(role) != roles_.end();
  } else {
    std::stack<const Grantee*> roles;
    roles.push(this);
    while (!roles.empty()) {
      auto r = roles.top();
      roles.pop();
      if (r == role) {
        return true;
      } else {
        for (auto granted_role : r->roles_) {
          roles.push(granted_role);
        }
      }
    }
    return false;
  }
}

void Grantee::getPrivileges(DBObject& object, bool only_direct) {
  auto dbObject = findDbObject(object.getObjectKey(), only_direct);
  if (!dbObject) {  // not found
    throw runtime_error("Can not get privileges because " + getName() +
                        " has no privileges to " + object.getName());
  }
  object.grantPrivileges(*dbObject);
}

/**
 * @brief 在 effectivePrivileges_/directPrivileges_ 查找object权限.
 * 
 * @param objectKey 
 * @param only_direct 
 * @return DBObject* 
 */
DBObject* Grantee::findDbObject(const DBObjectKey& objectKey, bool only_direct) const {
  const DBObjectMap& privs = only_direct ? directPrivileges_ : effectivePrivileges_;
  DBObject* dbObject = nullptr;
  auto dbObjectIt = privs.find(objectKey);
  if (dbObjectIt != privs.end()) {
    dbObject = dbObjectIt->second.get();
  }
  return dbObject;
}

bool Grantee::hasAnyPrivilegesOnDb(int32_t dbId, bool only_direct) const {
  const DBObjectMap& privs = only_direct ? directPrivileges_ : effectivePrivileges_;
  for (const auto& priv : privs) {
    if (priv.second->getObjectKey().dbId == dbId) {
      return true;
    }
  }
  return false;
}

/**
 * @brief 给 effectivePrivileges_/directPrivileges_ 添加权限 DBObject.objectPrivs_.
 * GRANT <privilegeList> ON TABLE <tableName> TO <entityList(User/Role)>;
 * 
 * @param object 
 */
void Grantee::grantPrivileges(const DBObject& object) {
  auto* dbObject = findDbObject(object.getObjectKey(), false);
  if (!dbObject) {  // not found
    effectivePrivileges_[object.getObjectKey()] = boost::make_unique<DBObject>(object);
  } else {  // found
    dbObject->grantPrivileges(object);
  }
  dbObject = findDbObject(object.getObjectKey(), true);
  if (!dbObject) {  // not found
    directPrivileges_[object.getObjectKey()] = boost::make_unique<DBObject>(object);
  } else {  // found
    dbObject->grantPrivileges(object);
  }
  updatePrivileges();
}

void Grantee::renameDbObject(const DBObject& object) {
  // rename direct and effective objects
  auto directIt = directPrivileges_.find(object.getObjectKey());
  if (directIt != directPrivileges_.end()) {
    directIt->second->setName(object.getName());
  }

  auto effectiveIt = effectivePrivileges_.find(object.getObjectKey());
  if (effectiveIt != effectivePrivileges_.end()) {
    effectiveIt->second->setName(object.getName());
  }
}
// Revoke privileges from the object.
// If there are no privileges left - erase object and return nullptr.
// Otherwise, return the changed object.
DBObject* Grantee::revokePrivileges(const DBObject& object) {
  auto dbObject = findDbObject(object.getObjectKey(), true);
  if (!dbObject ||
      !dbObject->getPrivileges().hasAny()) {  // not found or has none of privileges set
    throw runtime_error("Can not revoke privileges because " + getName() +
                        " has no privileges to " + object.getName());
  }
  bool object_removed = false;
  dbObject->revokePrivileges(object);
  if (!dbObject->getPrivileges().hasAny()) {
    directPrivileges_.erase(object.getObjectKey());
    object_removed = true;
  }

  auto* cachedDbObject = findDbObject(object.getObjectKey(), false);
  if (cachedDbObject && cachedDbObject->getPrivileges().hasAny()) {
    cachedDbObject->revokePrivileges(object);
    if (!cachedDbObject->getPrivileges().hasAny()) {
      effectivePrivileges_.erase(object.getObjectKey());
    }
  }

  updatePrivileges();

  return object_removed ? nullptr : dbObject;
}

/**
 * @brief 将role权限赋予给Grantee。
 * GRANT <roleNames> TO <userNames>, <roleNames>;
 */
void Grantee::grantRole(Role* role) {
  bool found = false;
  // 如果存在role就退出.
  for (const auto* granted_role : roles_) {
    if (role == granted_role) {
      found = true;
      break;
    }
  }
  if (found) {
    throw runtime_error("Role " + role->getName() + " have been granted to " + name_ +
                        " already.");
  }
  // 防止递归树形成回环。
  checkCycles(role);
  // 将role添加到树中。
  roles_.insert(role);
  role->addGrantee(this);
  // 虚函数实现，传播权限更新.
  updatePrivileges();
}

void Grantee::revokeRole(Role* role) {
  roles_.erase(role);
  role->removeGrantee(this);
  updatePrivileges();
}

static bool hasEnoughPrivs(const DBObject* real, const DBObject* requested) {
  if (real) {
    auto req = requested->getPrivileges().privileges;
    auto base = real->getPrivileges().privileges;

    // ensures that all requested privileges are present
    return req == (base & req);
  } else {
    return false;
  }
}

static bool hasAnyPrivs(const DBObject* real, const DBObject* /* requested*/) {
  if (real) {
    return real->getPrivileges().hasAny();
  } else {
    return false;
  }
}

bool Grantee::hasAnyPrivileges(const DBObject& objectRequested, bool only_direct) const {
  DBObjectKey objectKey = objectRequested.getObjectKey();
  if (hasAnyPrivs(findDbObject(objectKey, only_direct), &objectRequested)) {
    return true;
  }

  // if we have an object associated -> ignore it
  if (objectKey.objectId != -1) {
    objectKey.objectId = -1;
    if (hasAnyPrivs(findDbObject(objectKey, only_direct), &objectRequested)) {
      return true;
    }
  }

  // if we have an
  if (objectKey.dbId != -1) {
    objectKey.dbId = -1;
    if (hasAnyPrivs(findDbObject(objectKey, only_direct), &objectRequested)) {
      return true;
    }
  }
  return false;
}

bool Grantee::checkPrivileges(const DBObject& objectRequested) const {
  DBObjectKey objectKey = objectRequested.getObjectKey();
  if (hasEnoughPrivs(findDbObject(objectKey, false), &objectRequested)) {
    return true;
  }

  // if we have an object associated -> ignore it
  if (objectKey.objectId != -1) {
    objectKey.objectId = -1;
    if (hasEnoughPrivs(findDbObject(objectKey, false), &objectRequested)) {
      return true;
    }
  }

  // if we have an
  if (objectKey.dbId != -1) {
    objectKey.dbId = -1;
    if (hasEnoughPrivs(findDbObject(objectKey, false), &objectRequested)) {
      return true;
    }
  }
  return false;
}

/**
 * @brief 将Role的effectivePrivileges_全部添加到effectivePrivileges_.
 * 
 * @param role 
 */
void Grantee::updatePrivileges(Role* role) {
  for (auto& roleDbObject : *role->getDbObjects(false)) {
    auto dbObject = findDbObject(roleDbObject.first, false);
    if (dbObject) {  // found
      dbObject->updatePrivileges(*roleDbObject.second);
    } else {  // not found
      effectivePrivileges_[roleDbObject.first] =
          boost::make_unique<DBObject>(*roleDbObject.second.get());
    }
  }
}

/**
 * 重新将 directPrivileges_ 与 上游roles 权限更新到 effectivePrivileges_.
 * Pull privileges from upper roles  
 */
void Grantee::updatePrivileges() {
  // 清除 effectivePrivileges_ 所有权限.
  for (auto& dbObject : effectivePrivileges_) {
    dbObject.second->resetPrivileges();
  }
  // 将直接权限 directPrivileges_ 添加到 effectivePrivileges_.
  for (auto it = directPrivileges_.begin(); it != directPrivileges_.end(); ++it) {
    if (effectivePrivileges_.find(it->first) != effectivePrivileges_.end()) {
      effectivePrivileges_[it->first]->updatePrivileges(*it->second);
    }
  }
  // 将所有roles_权限更新到 effectivePrivileges_.
  for (auto role : roles_) {
    if (role->getDbObjects(false)->size() > 0) {
      updatePrivileges(role);
    }
  }
  for (auto dbObjectIt = effectivePrivileges_.begin();
       dbObjectIt != effectivePrivileges_.end();) {
    // 清除空的权限.
    if (!dbObjectIt->second->getPrivileges().hasAny()) {
      dbObjectIt = effectivePrivileges_.erase(dbObjectIt);
    } else {
      ++dbObjectIt;
    }
  }
}

void Grantee::revokeAllOnDatabase(int32_t dbId) {
  std::vector<DBObjectMap*> sources = {&effectivePrivileges_, &directPrivileges_};
  for (auto privs : sources) {
    for (auto iter = privs->begin(); iter != privs->end();) {
      if (iter->first.dbId == dbId) {
        iter = privs->erase(iter);
      } else {
        ++iter;
      }
    }
  }
  updatePrivileges();
}

/**
 * @brief bfs查找检测回环.
 * 
 * @param newRole 
 */
void Grantee::checkCycles(Role* newRole) {
  std::stack<Grantee*> grantees;
  grantees.push(this);
  while (!grantees.empty()) {
    auto* grantee = grantees.top();
    grantees.pop();
    if (!grantee->isUser()) {
      // 将grantee转换为role，遍历role的下游grantees.
      Role* r = dynamic_cast<Role*>(grantee);
      CHECK(r);
      // 下游子树不应该有上游出现的role.
      if (r == newRole) {
        throw runtime_error("Granting role " + newRole->getName() + " to " + getName() +
                            " creates cycle in grantee graph.");
      }
      for (auto g : r->getGrantees()) {
        grantees.push(g);
      }
    }
  }
}

void Grantee::reassignObjectOwners(const std::set<int32_t>& old_owner_ids,
                                   int32_t new_owner_id,
                                   int32_t db_id) {
  for (const auto& [object_key, object] : effectivePrivileges_) {
    if (object_key.objectId != -1 && object_key.dbId == db_id &&
        shared::contains(old_owner_ids, object->getOwner())) {
      object->setOwner(new_owner_id);
    }
  }

  for (const auto& [object_key, object] : directPrivileges_) {
    if (object_key.objectId != -1 && object_key.dbId == db_id &&
        shared::contains(old_owner_ids, object->getOwner())) {
      object->setOwner(new_owner_id);
    }
  }
}

void Grantee::reassignObjectOwner(DBObjectKey& object_key, int32_t new_owner_id) {
  for (const auto& [grantee_object_key, object] : effectivePrivileges_) {
    if (grantee_object_key == object_key) {
      object->setOwner(new_owner_id);
    }
  }

  for (const auto& [grantee_object_key, object] : directPrivileges_) {
    if (grantee_object_key == object_key) {
      object->setOwner(new_owner_id);
    }
  }
}

Role::~Role() {
  for (auto it = grantees_.begin(); it != grantees_.end();) {
    auto current_grantee = *it;
    ++it;
    current_grantee->revokeRole(this);
  }
  grantees_.clear();
}

/**
 * @brief 将 Grantee 作为 Role 下游节点. 注意不更新权限.
 * 
 * @param grantee 
 */
void Role::addGrantee(Grantee* grantee) {
  if (grantees_.find(grantee) == grantees_.end()) {
    grantees_.insert(grantee);
  } else {
    throw runtime_error("Role " + getName() + " have been granted to " +
                        grantee->getName() + " already.");
  }
}

void Role::removeGrantee(Grantee* grantee) {
  if (grantees_.find(grantee) != grantees_.end()) {
    grantees_.erase(grantee);
  } else {
    throw runtime_error("Role " + getName() + " have not been granted to " +
                        grantee->getName() + " .");
  }
}

std::vector<Grantee*> Role::getGrantees() const {
  std::vector<Grantee*> grantees;
  for (const auto grantee : grantees_) {
    grantees.push_back(grantee);
  }
  return grantees;
}

void Role::revokeAllOnDatabase(int32_t dbId) {
  Grantee::revokeAllOnDatabase(dbId);
  for (auto grantee : grantees_) {
    grantee->revokeAllOnDatabase(dbId);
  }
}

/**
 * @brief 更新本节点权限，并传播权限到下游的grantess.
 * Pull privileges from upper roles and push those to grantees
 * 
 */
void Role::updatePrivileges() {
  // 更新本节点role权限.
  Grantee::updatePrivileges();
  // 传递role权限到下游 grantee.
  for (auto grantee : grantees_) {
    grantee->updatePrivileges();
  }
}

void Role::renameDbObject(const DBObject& object) {
  Grantee::renameDbObject(object);
  for (auto grantee : grantees_) {
    grantee->renameDbObject(object);
  }
}
