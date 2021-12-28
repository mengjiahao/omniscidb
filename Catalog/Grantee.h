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

#ifndef GRANTEE_H
#define GRANTEE_H

#include "DBObject.h"

#include <boost/algorithm/string.hpp>
#include <boost/make_unique.hpp>
#include <map>
#include <string>
#include <unordered_set>
#include "Logger/Logger.h"

class User;
class Role;

/**
 * @brief 父类Grantee有多个Role，每个Role可能又有多个Grantee，因此权限是个递归树。
 * 
 * many-to-many relationships: Objects and roles, Objects and users, Roles and users.
 * Each object privilege can be granted to one or many roles, or to one or many users.
 * Granting object privileges to roles and users, and granting roles to users, has a cumulative effect. 
 * The result of several grant commands is a combination of all individual grant commands. 
 * This applies to all database object types and to privileges inherited by object.
 * For example, object privileges granted to the object of database type are propagated to all table-type objects of that database object.
 */
class Grantee {
  using DBObjectMap = std::map<DBObjectKey, std::unique_ptr<DBObject>>;

 public:
  Grantee(const std::string& name);
  virtual ~Grantee();
  virtual bool isUser() const = 0;
  virtual void grantPrivileges(const DBObject& object);
  virtual DBObject* revokePrivileges(const DBObject& object);
  virtual void grantRole(Role* role);
  virtual void revokeRole(Role* role);
  virtual bool hasAnyPrivileges(const DBObject& objectRequested, bool only_direct) const;
  virtual bool checkPrivileges(const DBObject& objectRequested) const;
  virtual void updatePrivileges();
  virtual void updatePrivileges(Role* role);
  virtual void revokeAllOnDatabase(int32_t dbId);
  virtual void renameDbObject(const DBObject& object);
  void getPrivileges(DBObject& object, bool only_direct);
  DBObject* findDbObject(const DBObjectKey& objectKey, bool only_direct) const;
  bool hasAnyPrivilegesOnDb(int32_t dbId, bool only_direct) const;
  const std::string& getName() const { return name_; }
  void setName(const std::string& name) { name_ = name; }
  std::vector<std::string> getRoles(bool only_direct = true) const;
  bool hasRole(Role* role, bool only_direct) const;
  const DBObjectMap* getDbObjects(bool only_direct) const {
    return only_direct ? &directPrivileges_ : &effectivePrivileges_;
  }
  void checkCycles(Role* newRole);

  void reassignObjectOwners(const std::set<int32_t>& old_owner_ids,
                            int32_t new_owner_id,
                            int32_t db_id);
  void reassignObjectOwner(DBObjectKey& object_key, int32_t new_owner_id);

 protected:
  std::string name_;
  // 父节点指针. grant role to grantee, 表示 roles_ 是 grantee的上游, role权限有更新会传递到 grantee, grantee拥有roles所有权限.
  std::unordered_set<Role*> roles_;
  // 所有权限. tracks all privileges, including privileges from granted roles recursively
  DBObjectMap effectivePrivileges_;
  // 此节点的直接权限. tracks only privileges granted directly to this grantee
  DBObjectMap directPrivileges_;
};

/**
 * @brief A user can be granted one or many roles. 只能将 role权限给user，不能grant user to role.
 * 
 */
class User : public Grantee {
 public:
  User(const std::string& name) : Grantee(name) {}
  bool isUser() const override { return true; }
};

/**
 * @brief Role可能有多个Grantee。
 * A role and/or user can be granted privileges on one or many objects.
 * A role can be granted to one or many users or other roles.
 */
class Role : public Grantee {
 public:
  Role(const std::string& name) : Grantee(name) {}
  ~Role() override;

  bool isUser() const override { return false; }
  void updatePrivileges() override;
  void renameDbObject(const DBObject& object) override;

  // NOTE(max): To be used only from Grantee. Grantee使用其组成权限树.
  virtual void addGrantee(Grantee* grantee);
  virtual void removeGrantee(Grantee* grantee);

  void revokeAllOnDatabase(int32_t dbId) override;
  std::vector<Grantee*> getGrantees() const;

 private:
  // 子节点指针. 这个用于传递role权限到下游的 grantees_, 下游grantees_ 拥有role的权限, 但是上游 role没有grantee的所有权限.
  std::unordered_set<Grantee*> grantees_;
};

#endif /* GRANTEE_H */
