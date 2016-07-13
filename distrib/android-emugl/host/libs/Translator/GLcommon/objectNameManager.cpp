/*
* Copyright (C) 2011 The Android Open Source Project
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/
#include <GLcommon/objectNameManager.h>
#include <GLcommon/GLEScontext.h>

#include <utility>

namespace {
// A struct serving as a key in a hash table, represents an object name with
// object type together.
struct TypedObjectName {
    ObjectLocalName name;
    NamedObjectType type;

    TypedObjectName(NamedObjectType type, ObjectLocalName name)
        : name(name), type(type) {}

    bool operator==(const TypedObjectName& other) const noexcept {
        return name == other.name && type == other.type;
    }
};
}  // namespace

namespace std {
template <>
struct hash<TypedObjectName> {
    size_t operator()(const TypedObjectName& tn) const noexcept {
        return hash<int>()(tn.name) ^
               hash<ObjectLocalName>()(tn.type);
    }
};
}  // namespace std

using ObjectDataMap = std::unordered_map<TypedObjectName, ObjectDataPtr>;
using TextureRefCounterMap = std::unordered_map<unsigned int, size_t>;

NameSpace::NameSpace(NamedObjectType p_type,
                     GlobalNameSpace *globalNameSpace) :
    m_type(p_type),
    m_globalNameSpace(globalNameSpace) {}

NameSpace::~NameSpace()
{
    for (NamesMap::iterator n = m_localToGlobalMap.begin();
         n != m_localToGlobalMap.end();
         ++n) {
        m_globalNameSpace->deleteName(m_type, (*n).second);
    }
}

ObjectLocalName
NameSpace::genName(ObjectLocalName p_localName,
                   bool genGlobal, bool genLocal)
{
    ObjectLocalName localName = p_localName;
    if (genLocal) {
        do {
            localName = ++m_nextName;
        } while(localName == 0 ||
                m_localToGlobalMap.find(localName) !=
                        m_localToGlobalMap.end() );
    }

    if (genGlobal) {
        unsigned int globalName = m_globalNameSpace->genName(m_type);
        m_localToGlobalMap[localName] = globalName;
        m_globalToLocalMap[globalName] = localName;
    }

    return localName;
}


unsigned int
NameSpace::genGlobalName(void)
{
    return m_globalNameSpace->genName(m_type);
}

unsigned int
NameSpace::getGlobalName(ObjectLocalName p_localName)
{
    NamesMap::iterator n( m_localToGlobalMap.find(p_localName) );
    if (n != m_localToGlobalMap.end()) {
        // object found - return its global name map
        return (*n).second;
    }

    // object does not exist;
    return 0;
}

ObjectLocalName
NameSpace::getLocalName(unsigned int p_globalName)
{
    const auto it = m_globalToLocalMap.find(p_globalName);
    if (it != m_globalToLocalMap.end()) {
        return it->second;
    }

    return 0;
}

void
NameSpace::deleteName(ObjectLocalName p_localName)
{
    NamesMap::iterator n( m_localToGlobalMap.find(p_localName) );
    if (n != m_localToGlobalMap.end()) {
        if (m_type != TEXTURE) {
            m_globalNameSpace->deleteName(m_type, (*n).second);
        }
        m_globalToLocalMap.erase(n->second);
        m_localToGlobalMap.erase(n);
    }
}

bool
NameSpace::isObject(ObjectLocalName p_localName)
{
    return (m_localToGlobalMap.find(p_localName) != m_localToGlobalMap.end() );
}

void
NameSpace::replaceGlobalName(ObjectLocalName p_localName, unsigned int p_globalName)
{
    NamesMap::iterator n( m_localToGlobalMap.find(p_localName) );
    if (n != m_localToGlobalMap.end()) {
        if (m_type != TEXTURE) {
            m_globalNameSpace->deleteName(m_type, (*n).second);
        }
        m_globalToLocalMap.erase(n->second);
        (*n).second = p_globalName;
        m_globalToLocalMap.emplace(p_globalName, p_localName);
    }
}

unsigned int 
GlobalNameSpace::genName(NamedObjectType p_type)
{
    if ( p_type >= NUM_OBJECT_TYPES ) return 0;
    unsigned int name = 0;

    emugl::Mutex::AutoLock _lock(m_lock);
    switch (p_type) {
    case VERTEXBUFFER:
        GLEScontext::dispatcher().glGenBuffers(1,&name);
        break;
    case TEXTURE:
        GLEScontext::dispatcher().glGenTextures(1,&name);
        break;
    case RENDERBUFFER:
        GLEScontext::dispatcher().glGenRenderbuffersEXT(1,&name);
        break;
    case FRAMEBUFFER:
        GLEScontext::dispatcher().glGenFramebuffersEXT(1,&name);
        break;
    case SHADER: //objects in shader namepace are not handled
    default:
        name = 0;
    }
    if (!m_deleteInitialized) {
        m_deleteInitialized = true;
        m_glDelete[VERTEXBUFFER] = GLEScontext::dispatcher().glDeleteBuffers;
        m_glDelete[TEXTURE] = GLEScontext::dispatcher().glDeleteTextures;
        m_glDelete[RENDERBUFFER] =
                GLEScontext::dispatcher().glDeleteRenderbuffersEXT;
        m_glDelete[FRAMEBUFFER] =
                GLEScontext::dispatcher().glDeleteFramebuffersEXT;
    }
    return name;
}

void 
GlobalNameSpace::deleteName(NamedObjectType p_type, unsigned int p_name)
{
    if (p_type != SHADER) {
        emugl::Mutex::AutoLock _lock(m_lock);
        m_glDelete[p_type](1, &p_name);
    }
}

ShareGroup::ShareGroup(GlobalNameSpace *globalNameSpace) {
    for (int i=0; i < NUM_OBJECT_TYPES; i++) {
        m_nameSpace[i] = new NameSpace((NamedObjectType)i, globalNameSpace);
    }
}

ShareGroup::~ShareGroup()
{
    emugl::Mutex::AutoLock _lock(m_lock);
    for (int t = 0; t < NUM_OBJECT_TYPES; t++) {
        delete m_nameSpace[t];
    }

    delete (ObjectDataMap *)m_objectsData;
    delete (TextureRefCounterMap *)m_globalTextureRefCounter;
}

ObjectLocalName
ShareGroup::genName(NamedObjectType p_type,
                    ObjectLocalName p_localName,
                    bool genLocal)
{
    if (p_type >= NUM_OBJECT_TYPES) return 0;

    emugl::Mutex::AutoLock _lock(m_lock);
    ObjectLocalName localName =
            m_nameSpace[p_type]->genName(p_localName, true, genLocal);
    if (p_type == TEXTURE) {
        incTexRefCounterNoLock(m_nameSpace[p_type]->getGlobalName(localName));
    }
    return localName;
}

unsigned int
ShareGroup::genGlobalName(NamedObjectType p_type)
{
    if (p_type >= NUM_OBJECT_TYPES) return 0;

    emugl::Mutex::AutoLock _lock(m_lock);
    return m_nameSpace[p_type]->genGlobalName();
}

unsigned int
ShareGroup::getGlobalName(NamedObjectType p_type,
                          ObjectLocalName p_localName)
{
    if (p_type >= NUM_OBJECT_TYPES) return 0;

    emugl::Mutex::AutoLock _lock(m_lock);
    return m_nameSpace[p_type]->getGlobalName(p_localName);
}

ObjectLocalName
ShareGroup::getLocalName(NamedObjectType p_type,
                         unsigned int p_globalName)
{
    if (p_type >= NUM_OBJECT_TYPES) return 0;

    emugl::Mutex::AutoLock _lock(m_lock);
    return m_nameSpace[p_type]->getLocalName(p_globalName);
}

void
ShareGroup::deleteName(NamedObjectType p_type, ObjectLocalName p_localName)
{
    if (p_type >= NUM_OBJECT_TYPES) return;

    emugl::Mutex::AutoLock _lock(m_lock);
    m_nameSpace[p_type]->deleteName(p_localName);
    ObjectDataMap *map = (ObjectDataMap *)m_objectsData;
    if (map) {
        map->erase(TypedObjectName(p_type, p_localName));
    }
}

bool
ShareGroup::isObject(NamedObjectType p_type, ObjectLocalName p_localName)
{
    if (p_type >= NUM_OBJECT_TYPES) return 0;

    emugl::Mutex::AutoLock _lock(m_lock);
    return m_nameSpace[p_type]->isObject(p_localName);
}

void
ShareGroup::replaceGlobalName(NamedObjectType p_type,
                              ObjectLocalName p_localName,
                              unsigned int p_globalName)
{
    if (p_type >= NUM_OBJECT_TYPES) return;

    emugl::Mutex::AutoLock _lock(m_lock);
    m_nameSpace[p_type]->replaceGlobalName(p_localName, p_globalName);
}

void
ShareGroup::setObjectData(NamedObjectType p_type,
                          ObjectLocalName p_localName,
                          ObjectDataPtr data)
{
    if (p_type >= NUM_OBJECT_TYPES) return;

    emugl::Mutex::AutoLock _lock(m_lock);

    ObjectDataMap *map = (ObjectDataMap *)m_objectsData;
    if (!map) {
        map = new ObjectDataMap();
        m_objectsData = map;
    }

    TypedObjectName id(p_type, p_localName);
    map->emplace(id, std::move(data));
}

ObjectDataPtr
ShareGroup::getObjectData(NamedObjectType p_type,
                          ObjectLocalName p_localName)
{
    ObjectDataPtr ret;

    if (p_type >= NUM_OBJECT_TYPES) return ret;

    emugl::Mutex::AutoLock _lock(m_lock);

    ObjectDataMap *map = (ObjectDataMap *)m_objectsData;
    if (map) {
        ObjectDataMap::iterator i =
                map->find(TypedObjectName(p_type, p_localName));
        if (i != map->end()) ret = (*i).second;
    }
    return ret;
}

size_t
ShareGroup::incTexRefCounterNoLock(unsigned int p_globalName) {
    TextureRefCounterMap *map =
            (TextureRefCounterMap *)m_globalTextureRefCounter;
    if (!map) {
        map = new TextureRefCounterMap();
        m_globalTextureRefCounter = map;
    }
    size_t& val = (*map)[p_globalName];
    val ++;
    return val;
}

size_t
ShareGroup::incTexRefCounter(unsigned int p_globalName) {
    emugl::Mutex::AutoLock _lock(m_lock);
    return incTexRefCounterNoLock(p_globalName);
}

size_t
ShareGroup::decTexRefCounterAndReleaseIf0(unsigned int p_globalName) {
    emugl::Mutex::AutoLock _lock(m_lock);
    assert(m_globalTextureRefCounter);
    TextureRefCounterMap *map =
            (TextureRefCounterMap *)m_globalTextureRefCounter;
    auto iterator = map->find(p_globalName);
    if (iterator == map->end()) {
        return 0;
    }
    size_t& val = iterator->second;
    assert(val != 0);
    val --;
    if (val) {
        return val;
    }
    map->erase(iterator);
    m_nameSpace[TEXTURE]->m_globalNameSpace->deleteName(TEXTURE, p_globalName);
    return 0;
}

ObjectNameManager::ObjectNameManager(GlobalNameSpace *globalNameSpace) :
    m_globalNameSpace(globalNameSpace) {}

ShareGroupPtr
ObjectNameManager::createShareGroup(void *p_groupName)
{
    emugl::Mutex::AutoLock _lock(m_lock);

    ShareGroupPtr shareGroupReturn;

    ShareGroupsMap::iterator s( m_groups.find(p_groupName) );
    if (s != m_groups.end()) {
        shareGroupReturn = (*s).second;
    } else {
        //
        // Group does not exist, create new group
        //
        shareGroupReturn = ShareGroupPtr(new ShareGroup(m_globalNameSpace));
        m_groups.emplace(p_groupName, shareGroupReturn);
    }

    return shareGroupReturn;
}

ShareGroupPtr
ObjectNameManager::getShareGroup(void *p_groupName)
{
    emugl::Mutex::AutoLock _lock(m_lock);

    ShareGroupPtr shareGroupReturn;

    ShareGroupsMap::iterator s( m_groups.find(p_groupName) );
    if (s != m_groups.end()) {
        shareGroupReturn = (*s).second;
    }

    return shareGroupReturn;
}

ShareGroupPtr
ObjectNameManager::attachShareGroup(void *p_groupName,
                                    void *p_existingGroupName)
{
    emugl::Mutex::AutoLock _lock(m_lock);

    ShareGroupsMap::iterator s( m_groups.find(p_existingGroupName) );
    if (s == m_groups.end()) {
        // ShareGroup is not found !!!
        return ShareGroupPtr();
    }

    ShareGroupPtr shareGroupReturn((*s).second);
    if (m_groups.find(p_groupName) == m_groups.end()) {
        m_groups.emplace(p_groupName, shareGroupReturn);
    }
    return shareGroupReturn;
}

void
ObjectNameManager::deleteShareGroup(void *p_groupName)
{
    emugl::Mutex::AutoLock _lock(m_lock);

    ShareGroupsMap::iterator s( m_groups.find(p_groupName) );
    if (s != m_groups.end()) {
        m_groups.erase(s);
    }
}

void *ObjectNameManager::getGlobalContext()
{
    emugl::Mutex::AutoLock _lock(m_lock);
    return m_groups.empty() ? nullptr : m_groups.begin()->first;
}
