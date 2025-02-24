/****************************************************************************
Copyright (c) 2014-2016 Chukong Technologies Inc.
Copyright (c) 2017-2018 Xiamen Yaji Software Co., Ltd.
Copyright (c) 2021 Bytedance Inc.

 https://axmolengine.github.io/

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
****************************************************************************/

#include "3d/Bundle3D.h"
#include "3d/ObjLoader.h"

#include "base/Macros.h"
#include "platform/FileUtils.h"
#include "3d/BundleReader.h"
#include "base/Data.h"

#define BUNDLE_TYPE_SCENE 1
#define BUNDLE_TYPE_NODE 2
#define BUNDLE_TYPE_ANIMATIONS 3
#define BUNDLE_TYPE_ANIMATION 4
#define BUNDLE_TYPE_ANIMATION_CHANNEL 5
#define BUNDLE_TYPE_MODEL 10
#define BUNDLE_TYPE_MATERIAL 16
#define BUNDLE_TYPE_EFFECT 18
#define BUNDLE_TYPE_CAMERA 32
#define BUNDLE_TYPE_LIGHT 33
#define BUNDLE_TYPE_MESH 34
#define BUNDLE_TYPE_MESHPART 35
#define BUNDLE_TYPE_MESHSKIN 36

static const char* VERSION       = "version";
static const char* ID            = "id";
static const char* DEFAULTPART   = "body";
static const char* VERTEXSIZE    = "vertexsize";
static const char* VERTEX        = "vertex";
static const char* VERTICES      = "vertices";
static const char* INDEXNUM      = "indexnum";
static const char* INDICES       = "indices";
static const char* SUBMESH       = "submesh";
static const char* ATTRIBUTES    = "attributes";
static const char* ATTRIBUTESIZE = "size";
static const char* TYPE          = "type";
static const char* ATTRIBUTE     = "attribute";
static const char* SKIN          = "skin";
static const char* BINDSHAPE     = "bindshape";
static const char* MESH          = "mesh";
static const char* MESHES        = "meshes";
static const char* MESHPARTID    = "meshpartid";
static const char* MATERIALID    = "materialid";
static const char* NODE          = "node";
static const char* NODES         = "nodes";
static const char* CHILDREN      = "children";
static const char* PARTS         = "parts";
static const char* BONES         = "bones";
static const char* SKELETON      = "skeleton";
static const char* MATERIALS     = "materials";
static const char* ANIMATIONS    = "animations";
static const char* TRANSFORM     = "transform";
static const char* OLDTRANSFORM  = "tansform";
static const char* ANIMATION     = "animation";
static const char* MATERIAL      = "material";
static const char* BASE          = "base";
static const char* FILENAME      = "filename";
static const char* TEXTURES      = "textures";
static const char* LENGTH        = "length";
static const char* BONEID        = "boneId";
static const char* KEYFRAMES     = "keyframes";
static const char* TRANSLATION   = "translation";
static const char* ROTATION      = "rotation";
static const char* SCALE         = "scale";
static const char* KEYTIME       = "keytime";
static const char* AABBS         = "aabb";

NS_AX_BEGIN

void getChildMap(std::map<int, std::vector<int>>& map, SkinData* skinData, const rapidjson::Value& val)
{
    if (!skinData)
        return;

    // get transform matrix
    Mat4 transform;
    const rapidjson::Value& parent_transform = val[OLDTRANSFORM];
    for (rapidjson::SizeType j = 0, size = parent_transform.Size(); j < size; ++j)
    {
        transform.m[j] = parent_transform[j].GetFloat();
    }

    // set origin matrices
    std::string parent_name = val[ID].GetString();
    int parent_name_index   = skinData->getSkinBoneNameIndex(parent_name);
    if (parent_name_index < 0)
    {
        skinData->addNodeBoneNames(parent_name);
        skinData->nodeBoneOriginMatrices.emplace_back(transform);
        parent_name_index = skinData->getBoneNameIndex(parent_name);
    }
    else if (parent_name_index < static_cast<int>(skinData->skinBoneNames.size()))
    {
        skinData->skinBoneOriginMatrices[parent_name_index] = transform;
    }

    // set root bone index
    if (skinData->rootBoneIndex < 0)
        skinData->rootBoneIndex = parent_name_index;

    if (!val.HasMember(CHILDREN))
        return;

    const rapidjson::Value& children = val[CHILDREN];
    for (rapidjson::SizeType i = 0, size = children.Size(); i < size; ++i)
    {
        // get child bone name
        const rapidjson::Value& child = children[i];

        std::string child_name = child[ID].GetString();
        int child_name_index   = skinData->getSkinBoneNameIndex(child_name);
        if (child_name_index < 0)
        {
            skinData->addNodeBoneNames(child_name);
            child_name_index = skinData->getBoneNameIndex(child_name);
        }

        map[parent_name_index].emplace_back(child_name_index);

        getChildMap(map, skinData, child);
    }
}

Bundle3D* Bundle3D::createBundle()
{
    auto bundle = new Bundle3D();
    return bundle;
}

void Bundle3D::destroyBundle(Bundle3D* bundle)
{
    delete bundle;
}

void Bundle3D::clear()
{
    if (_isBinary)
    {
        _binaryBuffer.clear();
        AX_SAFE_DELETE_ARRAY(_references);
    }
    else
    {
        _jsonBuffer.clear();
    }
}

bool Bundle3D::load(std::string_view path)
{
    if (path.empty())
        return false;

    if (_path == path)
        return true;

    getModelRelativePath(path);

    bool ret        = false;
    std::string ext = FileUtils::getInstance()->getFileExtension(path);
    if (ext == ".c3t")
    {
        _isBinary = false;
        ret       = loadJson(path);
    }
    else if (ext == ".c3b")
    {
        _isBinary = true;
        ret       = loadBinary(path);
    }
    else
    {
        AXLOG("warning: %s is invalid file formate", path.data());
    }

    ret ? (_path = path) : (_path = "");

    return ret;
}

bool Bundle3D::loadObj(MeshDatas& meshdatas,
                       MaterialDatas& materialdatas,
                       NodeDatas& nodedatas,
                       std::string_view fullPath,
                       const char* mtl_basepath)
{
    meshdatas.resetData();
    materialdatas.resetData();
    nodedatas.resetData();

    std::string mtlPath = "";
    if (mtl_basepath)
        mtlPath = mtl_basepath;
    else
        mtlPath = fullPath.substr(0, fullPath.find_last_of("\\/") + 1);

    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    auto ret = tinyobj::LoadObj(shapes, materials, fullPath.data(), mtlPath.c_str());
    if (ret.empty())
    {
        // fill data
        // convert material
        int i = 0;
        char str[20];
        std::string dir = "";
        auto last       = fullPath.rfind('/');
        if (last != std::string::npos)
            dir = fullPath.substr(0, last + 1);
        for (auto&& material : materials)
        {
            NMaterialData materialdata;

            NTextureData tex;
            tex.filename = material.diffuse_texname.empty() ? material.diffuse_texname : dir + material.diffuse_texname;
            tex.type     = NTextureData::Usage::Diffuse;
            tex.wrapS    = backend::SamplerAddressMode::CLAMP_TO_EDGE;
            tex.wrapT    = backend::SamplerAddressMode::CLAMP_TO_EDGE;

            snprintf(str, sizeof(str), "%d", ++i);
            materialdata.textures.emplace_back(tex);
            materialdata.id = str;
            material.name   = str;
            materialdatas.materials.emplace_back(materialdata);
        }

        // convert mesh
        i = 0;
        for (auto&& shape : shapes)
        {
            auto mesh          = shape.mesh;
            MeshData* meshdata = new MeshData();
            MeshVertexAttrib attrib;
            attrib.type = parseGLDataType("GL_FLOAT", 3);

            if (mesh.positions.size())
            {
                attrib.vertexAttrib = shaderinfos::VertexKey::VERTEX_ATTRIB_POSITION;
                meshdata->attribs.emplace_back(attrib);
            }
            bool hasnormal = false, hastex = false;
            if (mesh.normals.size())
            {
                hasnormal           = true;
                attrib.vertexAttrib = shaderinfos::VertexKey::VERTEX_ATTRIB_NORMAL;
                meshdata->attribs.emplace_back(attrib);
            }
            if (mesh.texcoords.size())
            {
                hastex              = true;
                attrib.type         = parseGLDataType("GL_FLOAT", 2);
                attrib.vertexAttrib = shaderinfos::VertexKey::VERTEX_ATTRIB_TEX_COORD;
                meshdata->attribs.emplace_back(attrib);
            }

            auto vertexNum = mesh.positions.size() / 3;
            for (unsigned int k = 0; k < vertexNum; ++k)
            {
                meshdata->vertex.emplace_back(mesh.positions[k * 3]);
                meshdata->vertex.emplace_back(mesh.positions[k * 3 + 1]);
                meshdata->vertex.emplace_back(mesh.positions[k * 3 + 2]);

                if (hasnormal)
                {
                    meshdata->vertex.emplace_back(mesh.normals[k * 3]);
                    meshdata->vertex.emplace_back(mesh.normals[k * 3 + 1]);
                    meshdata->vertex.emplace_back(mesh.normals[k * 3 + 2]);
                }

                if (hastex)
                {
                    meshdata->vertex.emplace_back(mesh.texcoords[k * 2]);
                    meshdata->vertex.emplace_back(mesh.texcoords[k * 2 + 1]);
                }
            }

            // split into submesh according to material
            std::map<int, IndexArray> subMeshMap;
            for (size_t k = 0, size = mesh.material_ids.size(); k < size; ++k)
            {
                int id     = mesh.material_ids[k];
                size_t idx = k * 3;
                subMeshMap[id].emplace_back(mesh.indices[idx]);
                subMeshMap[id].emplace_back(mesh.indices[idx + 1]);
                subMeshMap[id].emplace_back(mesh.indices[idx + 2]);
            }

            auto node = new NodeData();
            node->id  = shape.name;
            for (auto&& submesh : subMeshMap)
            {
                auto& storedIndices = meshdata->subMeshIndices.emplace_back(std::move(submesh.second));
                meshdata->subMeshAABB.emplace_back(
                    calculateAABB(meshdata->vertex, meshdata->getPerVertexSize(), storedIndices));
                snprintf(str, sizeof(str), "%d", ++i);
                meshdata->subMeshIds.emplace_back(str);

                auto modelnode        = new ModelData();
                modelnode->materialId = submesh.first == -1 ? "" : materials[submesh.first].name;
                modelnode->subMeshId  = str;
                node->modelNodeDatas.emplace_back(modelnode);
            }
            nodedatas.nodes.emplace_back(node);
            meshdatas.meshDatas.emplace_back(meshdata);
        }

        return true;
    }
    AXLOG("warning: load %s file error: %s", fullPath.data(), ret.c_str());
    return false;
}

bool Bundle3D::loadSkinData(std::string_view /*id*/, SkinData* skindata)
{
    skindata->resetData();

    if (_isBinary)
    {
        return loadSkinDataBinary(skindata);
    }
    else
    {
        return loadSkinDataJson(skindata);
    }
}

bool Bundle3D::loadAnimationData(std::string_view id, Animation3DData* animationdata)
{
    animationdata->resetData();

    if (_isBinary)
    {
        return loadAnimationDataBinary(id, animationdata);
    }
    else
    {
        return loadAnimationDataJson(id, animationdata);
    }
}

// since 3.3, to support reskin
bool Bundle3D::loadMeshDatas(MeshDatas& meshdatas)
{
    meshdatas.resetData();
    if (_isBinary)
    {
        if (_version == "0.1" || _version == "0.2")
        {
            return loadMeshDatasBinary_0_1(meshdatas);
        }
        else
        {
            return loadMeshDatasBinary(meshdatas);
        }
    }
    else
    {
        if (_version == "1.2" || _version == "0.2")
        {
            return loadMeshDataJson_0_1(meshdatas);
        }
        else
        {
            return loadMeshDatasJson(meshdatas);
        }
    }
    return true;
}
bool Bundle3D::loadMeshDatasBinary(MeshDatas& meshdatas)
{
    if (!seekToFirstType(BUNDLE_TYPE_MESH))
        return false;
    unsigned int meshSize = 0;
    if (_binaryReader.read(&meshSize, 4, 1) != 1)
    {
        AXLOG("warning: Failed to read meshdata: attribCount '%s'.", _path.c_str());
        return false;
    }
    MeshData* meshData = nullptr;
    for (unsigned int i = 0; i < meshSize; ++i)
    {
        unsigned int attribSize = 0;
        // read mesh data
        if (_binaryReader.read(&attribSize, 4, 1) != 1 || attribSize < 1)
        {
            AXLOG("warning: Failed to read meshdata: attribCount '%s'.", _path.c_str());
            goto FAILED;
        }
        meshData              = new MeshData();
        meshData->attribCount = attribSize;
        meshData->attribs.resize(meshData->attribCount);
        for (ssize_t j = 0; j < meshData->attribCount; ++j)
        {
            std::string attribute = "";
            unsigned int vSize;
            if (_binaryReader.read(&vSize, 4, 1) != 1)
            {
                AXLOG("warning: Failed to read meshdata: usage or size '%s'.", _path.c_str());
                goto FAILED;
            }
            std::string type                  = _binaryReader.readString();
            attribute                         = _binaryReader.readString();
            meshData->attribs[j].type         = parseGLDataType(type, vSize);
            meshData->attribs[j].vertexAttrib = parseGLProgramAttribute(attribute);
        }
        unsigned int vertexSizeInFloat = 0;
        // Read vertex data
        if (_binaryReader.read(&vertexSizeInFloat, 4, 1) != 1 || vertexSizeInFloat == 0)
        {
            AXLOG("warning: Failed to read meshdata: vertexSizeInFloat '%s'.", _path.c_str());
            goto FAILED;
        }

        meshData->vertex.resize(vertexSizeInFloat);
        if (_binaryReader.read(&meshData->vertex[0], 4, vertexSizeInFloat) != vertexSizeInFloat)
        {
            AXLOG("warning: Failed to read meshdata: vertex element '%s'.", _path.c_str());
            goto FAILED;
        }

        // Read index data
        unsigned int meshPartCount = 1;
        _binaryReader.read(&meshPartCount, 4, 1);

        for (unsigned int k = 0; k < meshPartCount; ++k)
        {
            IndexArray indexArray{};
            std::string meshPartid = _binaryReader.readString();
            meshData->subMeshIds.emplace_back(meshPartid);
            unsigned int nIndexCount;
            if (_binaryReader.read(&nIndexCount, 4, 1) != 1)
            {
                AXLOG("warning: Failed to read meshdata: nIndexCount '%s'.", _path.c_str());
                goto FAILED;
            }
            indexArray.resize(nIndexCount);
            if (_binaryReader.read(indexArray.data(), 2, nIndexCount) != nIndexCount)
            {
                AXLOG("warning: Failed to read meshdata: indices '%s'.", _path.c_str());
                goto FAILED;
            }
            auto& storedIndices = meshData->subMeshIndices.emplace_back(std::move(indexArray));
            meshData->numIndex = (int)meshData->subMeshIndices.size();
            // meshData->subMeshAABB.emplace_back(calculateAABB(meshData->vertex, meshData->getPerVertexSize(),
            // indexArray));
            if (_version != "0.3" && _version != "0.4" && _version != "0.5")
            {
                // read mesh aabb
                float aabb[6];
                if (_binaryReader.read(aabb, 4, 6) != 6)
                {
                    AXLOG("warning: Failed to read meshdata: aabb '%s'.", _path.c_str());
                    goto FAILED;
                } 
                meshData->subMeshAABB.emplace_back(AABB(Vec3(aabb[0], aabb[1], aabb[2]), Vec3(aabb[3], aabb[4], aabb[5])));
            }
            else
            {
                meshData->subMeshAABB.emplace_back(
                    calculateAABB(meshData->vertex, meshData->getPerVertexSize(), storedIndices));
            }
        }
        meshdatas.meshDatas.emplace_back(meshData);
    }
    return true;

FAILED:
{
    AX_SAFE_DELETE(meshData);
    for (auto&& meshdata : meshdatas.meshDatas)
    {
        delete meshdata;
    }
    meshdatas.meshDatas.clear();
    return false;
}
}
bool Bundle3D::loadMeshDatasBinary_0_1(MeshDatas& meshdatas)
{
    if (!seekToFirstType(BUNDLE_TYPE_MESH))
        return false;

    meshdatas.resetData();

    MeshData* meshdata = new MeshData();

    // read mesh data
    unsigned int attribSize = 0;
    if (_binaryReader.read(&attribSize, 4, 1) != 1 || attribSize < 1)
    {
        AXLOG("warning: Failed to read meshdata: attribCount '%s'.", _path.c_str());
        AX_SAFE_DELETE(meshdata);
        return false;
    }
    enum
    {
        VERTEX_ATTRIB_POSITION,
        VERTEX_ATTRIB_COLOR,
        VERTEX_ATTRIB_TEX_COORD,
        VERTEX_ATTRIB_NORMAL,
        VERTEX_ATTRIB_BLEND_WEIGHT,
        VERTEX_ATTRIB_BLEND_INDEX,

        VERTEX_ATTRIB_MAX,

        // backward compatibility
        VERTEX_ATTRIB_TEX_COORDS = VERTEX_ATTRIB_TEX_COORD,
    };
    for (unsigned int i = 0; i < attribSize; ++i)
    {
        unsigned int vUsage, vSize;
        shaderinfos::VertexKey usage;
        if (_binaryReader.read(&vUsage, 4, 1) != 1 || _binaryReader.read(&vSize, 4, 1) != 1)
        {
            AXLOG("warning: Failed to read meshdata: usage or size '%s'.", _path.c_str());
            AX_SAFE_DELETE(meshdata);
            return false;
        }

        MeshVertexAttrib meshVertexAttribute;
        meshVertexAttribute.type = parseGLDataType("GL_FLOAT", vSize);
        if (vUsage == VERTEX_ATTRIB_NORMAL)
        {
            usage = shaderinfos::VertexKey::VERTEX_ATTRIB_NORMAL;
        }
        else if (vUsage == VERTEX_ATTRIB_BLEND_WEIGHT)
        {
            usage = shaderinfos::VertexKey::VERTEX_ATTRIB_BLEND_WEIGHT;
        }
        else if (vUsage == VERTEX_ATTRIB_BLEND_INDEX)
        {
            usage = shaderinfos::VertexKey::VERTEX_ATTRIB_BLEND_INDEX;
        }
        else if (vUsage == VERTEX_ATTRIB_POSITION)
        {
            usage = shaderinfos::VertexKey::VERTEX_ATTRIB_POSITION;
        }
        else if (vUsage == VERTEX_ATTRIB_TEX_COORD)
        {
            usage = shaderinfos::VertexKey::VERTEX_ATTRIB_TEX_COORD;
        }
        else
        {
            AXASSERT(false, "invalidate usage value");
        }
        meshVertexAttribute.vertexAttrib = usage;

        meshdata->attribs.emplace_back(meshVertexAttribute);
    }

    // Read vertex data
    if (_binaryReader.read(&meshdata->vertexSizeInFloat, 4, 1) != 1 || meshdata->vertexSizeInFloat == 0)
    {
        AXLOG("warning: Failed to read meshdata: vertexSizeInFloat '%s'.", _path.c_str());
        AX_SAFE_DELETE(meshdata);
        return false;
    }

    meshdata->vertex.resize(meshdata->vertexSizeInFloat);
    if (_binaryReader.read(&meshdata->vertex[0], 4, meshdata->vertexSizeInFloat) != meshdata->vertexSizeInFloat)
    {
        AXLOG("warning: Failed to read meshdata: vertex element '%s'.", _path.c_str());
        AX_SAFE_DELETE(meshdata);
        return false;
    }

    // Read index data
    unsigned int meshPartCount = 1;
    for (unsigned int i = 0; i < meshPartCount; ++i)
    {
        unsigned int nIndexCount;
        if (_binaryReader.read(&nIndexCount, 4, 1) != 1)
        {
            AXLOG("warning: Failed to read meshdata: nIndexCount '%s'.", _path.c_str());
            AX_SAFE_DELETE(meshdata);
            return false;
        }

        IndexArray indices{};
        indices.resize(nIndexCount);
        if (_binaryReader.read(indices.data(), 2, nIndexCount) != nIndexCount)
        {
            AXLOG("warning: Failed to read meshdata: indices '%s'.", _path.c_str());
            AX_SAFE_DELETE(meshdata);
            return false;
        }

        auto& storedIndices = meshdata->subMeshIndices.emplace_back(std::move(indices));
        meshdata->subMeshAABB.emplace_back(calculateAABB(meshdata->vertex, meshdata->getPerVertexSize(), storedIndices));
    }

    meshdatas.meshDatas.emplace_back(meshdata);
    return true;
}

bool Bundle3D::loadMeshDatasBinary_0_2(MeshDatas& meshdatas)
{
    if (!seekToFirstType(BUNDLE_TYPE_MESH))
        return false;

    meshdatas.resetData();

    MeshData* meshdata = new MeshData();

    // read mesh data
    unsigned int attribSize = 0;
    if (_binaryReader.read(&attribSize, 4, 1) != 1 || attribSize < 1)
    {
        AXLOG("warning: Failed to read meshdata: attribCount '%s'.", _path.c_str());
        AX_SAFE_DELETE(meshdata);
        return false;
    }
    enum
    {
        VERTEX_ATTRIB_POSITION,
        VERTEX_ATTRIB_COLOR,
        VERTEX_ATTRIB_TEX_COORD,
        VERTEX_ATTRIB_NORMAL,
        VERTEX_ATTRIB_BLEND_WEIGHT,
        VERTEX_ATTRIB_BLEND_INDEX,

        VERTEX_ATTRIB_MAX,

        // backward compatibility
        VERTEX_ATTRIB_TEX_COORDS = VERTEX_ATTRIB_TEX_COORD,
    };
    for (unsigned int i = 0; i < attribSize; ++i)
    {
        unsigned int vUsage, vSize;
        shaderinfos::VertexKey usage = shaderinfos::VertexKey::VERTEX_ATTRIB_ERROR;
        if (_binaryReader.read(&vUsage, 4, 1) != 1 || _binaryReader.read(&vSize, 4, 1) != 1)
        {
            AXLOG("warning: Failed to read meshdata: usage or size '%s'.", _path.c_str());
            AX_SAFE_DELETE(meshdata);
            return false;
        }

        MeshVertexAttrib meshVertexAttribute;
        meshVertexAttribute.type = parseGLDataType("GL_FLOAT", vSize);
        if (vUsage == VERTEX_ATTRIB_NORMAL)
        {
            usage = shaderinfos::VertexKey::VERTEX_ATTRIB_NORMAL;
        }
        else if (vUsage == VERTEX_ATTRIB_BLEND_WEIGHT)
        {
            usage = shaderinfos::VertexKey::VERTEX_ATTRIB_BLEND_WEIGHT;
        }
        else if (vUsage == VERTEX_ATTRIB_BLEND_INDEX)
        {
            usage = shaderinfos::VertexKey::VERTEX_ATTRIB_BLEND_INDEX;
        }
        else if (vUsage == VERTEX_ATTRIB_POSITION)
        {
            usage = shaderinfos::VertexKey::VERTEX_ATTRIB_POSITION;
        }
        else if (vUsage == VERTEX_ATTRIB_TEX_COORD)
        {
            usage = shaderinfos::VertexKey::VERTEX_ATTRIB_TEX_COORD;
        }
        meshVertexAttribute.vertexAttrib = usage;

        meshdata->attribs.emplace_back(meshVertexAttribute);
    }

    // Read vertex data
    if (_binaryReader.read(&meshdata->vertexSizeInFloat, 4, 1) != 1 || meshdata->vertexSizeInFloat == 0)
    {
        AXLOG("warning: Failed to read meshdata: vertexSizeInFloat '%s'.", _path.c_str());
        AX_SAFE_DELETE(meshdata);
        return false;
    }

    meshdata->vertex.resize(meshdata->vertexSizeInFloat);
    if (_binaryReader.read(&meshdata->vertex[0], 4, meshdata->vertexSizeInFloat) != meshdata->vertexSizeInFloat)
    {
        AXLOG("warning: Failed to read meshdata: vertex element '%s'.", _path.c_str());
        AX_SAFE_DELETE(meshdata);
        return false;
    }

    // read submesh
    unsigned int submeshCount;
    if (_binaryReader.read(&submeshCount, 4, 1) != 1)
    {
        AXLOG("warning: Failed to read meshdata: submeshCount '%s'.", _path.c_str());
        AX_SAFE_DELETE(meshdata);
        return false;
    }

    for (unsigned int i = 0; i < submeshCount; ++i)
    {
        unsigned int nIndexCount;
        if (_binaryReader.read(&nIndexCount, 4, 1) != 1)
        {
            AXLOG("warning: Failed to read meshdata: nIndexCount '%s'.", _path.c_str());
            AX_SAFE_DELETE(meshdata);
            return false;
        }

        IndexArray indices{}; /* TODO: _version == 1.3 use U_INT?*/
        indices.resize(nIndexCount);
        if (_binaryReader.read(indices.data(), 2, nIndexCount) != nIndexCount)
        {
            AXLOG("warning: Failed to read meshdata: indices '%s'.", _path.c_str());
            AX_SAFE_DELETE(meshdata);
            return false;
        }

        auto& storedIndices = meshdata->subMeshIndices.emplace_back(std::move(indices));
        meshdata->subMeshAABB.emplace_back(calculateAABB(meshdata->vertex, meshdata->getPerVertexSize(), storedIndices));
    }

    meshdatas.meshDatas.emplace_back(meshdata);

    return true;
}
bool Bundle3D::loadMeshDatasJson(MeshDatas& meshdatas)
{
    const rapidjson::Value& mesh_data_array = _jsonReader[MESHES];
    for (rapidjson::SizeType index = 0, mesh_data_array_size = mesh_data_array.Size(); index < mesh_data_array_size;
         ++index)
    {
        MeshData* meshData                = new MeshData();
        const rapidjson::Value& mesh_data = mesh_data_array[index];
        // mesh_vertex_attribute
        const rapidjson::Value& mesh_vertex_attribute = mesh_data[ATTRIBUTES];
        MeshVertexAttrib tempAttrib;
        meshData->attribCount = mesh_vertex_attribute.Size();
        meshData->attribs.resize(meshData->attribCount);
        for (rapidjson::SizeType i = 0, mesh_vertex_attribute_size = mesh_vertex_attribute.Size();
             i < mesh_vertex_attribute_size; ++i)
        {
            const rapidjson::Value& mesh_vertex_attribute_val = mesh_vertex_attribute[i];

            int size              = mesh_vertex_attribute_val[ATTRIBUTESIZE].GetInt();
            std::string type      = mesh_vertex_attribute_val[TYPE].GetString();
            std::string attribute = mesh_vertex_attribute_val[ATTRIBUTE].GetString();

            tempAttrib.type         = parseGLDataType(type, size);
            tempAttrib.vertexAttrib = parseGLProgramAttribute(attribute);
            meshData->attribs[i]    = tempAttrib;
        }
        // mesh vertices
        ////////////////////////////////////////////////////////////////////////////////////////////////
        const rapidjson::Value& mesh_data_vertex_array = mesh_data[VERTICES];
        auto mesh_data_vertex_array_size               = mesh_data_vertex_array.Size();
        meshData->vertexSizeInFloat                    = mesh_data_vertex_array_size;
        for (rapidjson::SizeType i = 0; i < mesh_data_vertex_array_size; ++i)
        {
            meshData->vertex.emplace_back(mesh_data_vertex_array[i].GetFloat());
        }
        // mesh part
        ////////////////////////////////////////////////////////////////////////////////////////////////
        const rapidjson::Value& mesh_part_array = mesh_data[PARTS];
        for (rapidjson::SizeType i = 0, mesh_part_array_size = mesh_part_array.Size(); i < mesh_part_array_size; ++i)
        {
            IndexArray indexArray{};
            const rapidjson::Value& mesh_part = mesh_part_array[i];
            meshData->subMeshIds.emplace_back(mesh_part[ID].GetString());
            // index_number
            const rapidjson::Value& indices_val_array = mesh_part[INDICES];
            for (rapidjson::SizeType j = 0, indices_val_array_size = indices_val_array.Size();
                 j < indices_val_array_size; ++j)
                indexArray.emplace_back((unsigned short)indices_val_array[j].GetUint());

            auto& storedIndices = meshData->subMeshIndices.emplace_back(std::move(indexArray));
            meshData->numIndex = (int)meshData->subMeshIndices.size();

            if (mesh_data.HasMember(AABBS))
            {
                const rapidjson::Value& mesh_part_aabb = mesh_part[AABBS];
                if (mesh_part.HasMember(AABBS) && mesh_part_aabb.Size() == 6)
                {
                    Vec3 min(mesh_part_aabb[(rapidjson::SizeType)0].GetFloat(),
                             mesh_part_aabb[(rapidjson::SizeType)1].GetFloat(),
                             mesh_part_aabb[(rapidjson::SizeType)2].GetFloat());
                    Vec3 max(mesh_part_aabb[(rapidjson::SizeType)3].GetFloat(),
                             mesh_part_aabb[(rapidjson::SizeType)4].GetFloat(),
                             mesh_part_aabb[(rapidjson::SizeType)5].GetFloat());
                    meshData->subMeshAABB.emplace_back(AABB(min, max));
                }
                else
                {
                    meshData->subMeshAABB.emplace_back(
                        calculateAABB(meshData->vertex, meshData->getPerVertexSize(), storedIndices));
                }
            }
            else
            {
                meshData->subMeshAABB.emplace_back(calculateAABB(meshData->vertex, meshData->getPerVertexSize(), storedIndices));
            }
        }
        meshdatas.meshDatas.emplace_back(meshData);
    }
    return true;
}
bool Bundle3D::loadNodes(NodeDatas& nodedatas)
{
    if (_version == "0.1" || _version == "1.2" || _version == "0.2")
    {
        SkinData skinData;
        if (!loadSkinData("", &skinData))
        {
            auto node             = new NodeData();
            auto modelnode        = new ModelData();
            modelnode->materialId = "";
            modelnode->subMeshId  = "";
            node->modelNodeDatas.emplace_back(modelnode);
            nodedatas.nodes.emplace_back(node);
            return true;
        }

        auto nodeDatas = new NodeData*[skinData.skinBoneNames.size() + skinData.nodeBoneNames.size()];
        int index      = 0;
        size_t i;
        auto skinBoneSize = skinData.skinBoneNames.size();
        auto nodeBoneSize = skinData.nodeBoneNames.size();
        for (i = 0; i < skinBoneSize; ++i)
        {
            nodeDatas[index]            = new NodeData();
            nodeDatas[index]->id        = skinData.skinBoneNames[i];
            nodeDatas[index]->transform = skinData.skinBoneOriginMatrices[i];
            ++index;
        }
        for (i = 0; i < nodeBoneSize; ++i)
        {
            nodeDatas[index]            = new NodeData();
            nodeDatas[index]->id        = skinData.nodeBoneNames[i];
            nodeDatas[index]->transform = skinData.nodeBoneOriginMatrices[i];
            ++index;
        }
        for (const auto& it : skinData.boneChild)
        {
            const auto& children = it.second;
            auto parent          = nodeDatas[it.first];
            for (const auto& child : children)
            {
                parent->children.emplace_back(nodeDatas[child]);
            }
        }
        nodedatas.skeleton.emplace_back(nodeDatas[skinData.rootBoneIndex]);
        auto node              = new NodeData();
        auto modelnode         = new ModelData();
        modelnode->materialId  = "";
        modelnode->subMeshId   = "";
        modelnode->bones       = skinData.skinBoneNames;
        modelnode->invBindPose = skinData.inverseBindPoseMatrices;
        node->modelNodeDatas.emplace_back(modelnode);
        nodedatas.nodes.emplace_back(node);
        delete[] nodeDatas;
    }
    else
    {
        if (_isBinary)
        {
            loadNodesBinary(nodedatas);
        }
        else
        {
            loadNodesJson(nodedatas);
        }
    }
    return true;
}
bool Bundle3D::loadMaterials(MaterialDatas& materialdatas)
{
    materialdatas.resetData();
    if (_isBinary)
    {
        if (_version == "0.1")
        {
            return loadMaterialsBinary_0_1(materialdatas);
        }
        else if (_version == "0.2")
        {
            return loadMaterialsBinary_0_2(materialdatas);
        }
        else
        {
            return loadMaterialsBinary(materialdatas);
        }
    }
    else
    {
        if (_version == "1.2")
        {
            return loadMaterialDataJson_0_1(materialdatas);
        }
        else if (_version == "0.2")
        {
            return loadMaterialDataJson_0_2(materialdatas);
        }
        else
        {
            return loadMaterialsJson(materialdatas);
        }
    }
    return true;
}
bool Bundle3D::loadMaterialsBinary(MaterialDatas& materialdatas)
{
    if (!seekToFirstType(BUNDLE_TYPE_MATERIAL))
        return false;
    unsigned int materialnum = 1;
    _binaryReader.read(&materialnum, 4, 1);
    for (unsigned int i = 0; i < materialnum; ++i)
    {
        NMaterialData materialData;
        materialData.id = _binaryReader.readString();

        // skip: diffuse(3), ambient(3), emissive(3), opacity(1), specular(3), shininess(1)
        float data[14];
        _binaryReader.read(&data, sizeof(float), 14);

        unsigned int textureNum = 1;
        _binaryReader.read(&textureNum, 4, 1);
        for (unsigned int j = 0; j < textureNum; ++j)
        {
            NTextureData textureData;
            textureData.id = _binaryReader.readString();
            if (textureData.id.empty())
            {
                AXLOG("warning: Failed to read Materialdata: texturePath is empty '%s'.", textureData.id.c_str());
                return false;
            }
            std::string texturePath = _binaryReader.readString();
            if (texturePath.empty())
            {
                AXLOG("warning: Failed to read Materialdata: texturePath is empty '%s'.", _path.c_str());
                return false;
            }

            textureData.filename = texturePath.empty() ? texturePath : _modelPath + texturePath;
            float uvdata[4];
            _binaryReader.read(&uvdata, sizeof(float), 4);
            textureData.type  = parseGLTextureType(_binaryReader.readString());
            textureData.wrapS = parseSamplerAddressMode(_binaryReader.readString());
            textureData.wrapT = parseSamplerAddressMode(_binaryReader.readString());
            materialData.textures.emplace_back(textureData);
        }
        materialdatas.materials.emplace_back(materialData);
    }
    return true;
}
bool Bundle3D::loadMaterialsBinary_0_1(MaterialDatas& materialdatas)
{
    if (!seekToFirstType(BUNDLE_TYPE_MATERIAL))
        return false;

    NMaterialData materialData;

    std::string texturePath = _binaryReader.readString();
    if (texturePath.empty())
    {
        AXLOG("warning: Failed to read Materialdata: texturePath is empty '%s'.", _path.c_str());
        return false;
    }

    NTextureData textureData;
    textureData.filename = texturePath.empty() ? texturePath : _modelPath + texturePath;
    textureData.type     = NTextureData::Usage::Diffuse;
    textureData.id       = "";
    materialData.textures.emplace_back(textureData);
    materialdatas.materials.emplace_back(materialData);
    return true;
}

bool Bundle3D::loadMaterialsBinary_0_2(MaterialDatas& materialdatas)
{
    if (!seekToFirstType(BUNDLE_TYPE_MATERIAL))
        return false;

    unsigned int materialnum = 1;
    _binaryReader.read(&materialnum, 4, 1);

    for (unsigned int i = 0; i < materialnum; ++i)
    {
        NMaterialData materialData;

        std::string texturePath = _binaryReader.readString();
        if (texturePath.empty())
        {
            AXLOG("warning: Failed to read Materialdata: texturePath is empty '%s'.", _path.c_str());
            return true;
        }

        NTextureData textureData;
        textureData.filename = texturePath.empty() ? texturePath : _modelPath + texturePath;
        textureData.type     = NTextureData::Usage::Diffuse;
        textureData.id       = "";
        materialData.textures.emplace_back(textureData);
        materialdatas.materials.emplace_back(materialData);
    }
    return true;
}

bool loadMeshDataJson(MeshData* /*meshdata*/)
{
    return true;
}

bool loadMeshDataJson_0_1(MeshData* /*meshdata*/)
{
    return true;
}

bool loadMeshDataJson_0_2(MeshData* /*meshdata*/)
{
    return true;
}

bool Bundle3D::loadMaterialsJson(MaterialDatas& materialdatas)
{
    if (!_jsonReader.HasMember(MATERIALS))
        return false;
    const rapidjson::Value& material_array = _jsonReader[MATERIALS];
    for (rapidjson::SizeType i = 0; i < material_array.Size(); ++i)
    {
        NMaterialData materialData;
        const rapidjson::Value& material_val = material_array[i];
        materialData.id                      = material_val[ID].GetString();
        if (material_val.HasMember(TEXTURES))
        {
            const rapidjson::Value& texture_array = material_val[TEXTURES];
            for (rapidjson::SizeType j = 0; j < texture_array.Size(); ++j)
            {
                NTextureData textureData;
                const rapidjson::Value& texture_val = texture_array[j];
                std::string filename                = texture_val[FILENAME].GetString();
                textureData.filename                = filename.empty() ? filename : _modelPath + filename;
                textureData.type                    = parseGLTextureType(texture_val["type"].GetString());
                textureData.wrapS                   = parseSamplerAddressMode(texture_val["wrapModeU"].GetString());
                textureData.wrapT                   = parseSamplerAddressMode(texture_val["wrapModeV"].GetString());
                materialData.textures.emplace_back(textureData);
            }
        }
        materialdatas.materials.emplace_back(materialData);
    }
    return true;
}
bool Bundle3D::loadJson(std::string_view path)
{
    clear();

    _jsonBuffer = FileUtils::getInstance()->getStringFromFile(path);

    if (_jsonReader.ParseInsitu<0>((char*)_jsonBuffer.c_str()).HasParseError())
    {
        clear();
        AXLOG("Parse json failed in Bundle3D::loadJson function");
        return false;
    }

    const rapidjson::Value& mash_data_array = _jsonReader[VERSION];
    if (mash_data_array.IsArray())  // Compatible with the old version
        _version = "1.2";
    else
        _version = mash_data_array.GetString();

    return true;
}

bool Bundle3D::loadBinary(std::string_view path)
{
    clear();

    // get file data
    _binaryBuffer.clear();
    _binaryBuffer = FileUtils::getInstance()->getDataFromFile(path);
    if (_binaryBuffer.isNull())
    {
        clear();
        AXLOG("warning: Failed to read file: %s", path.data());
        return false;
    }

    // Initialise bundle reader
    _binaryReader.init((char*)_binaryBuffer.getBytes(), _binaryBuffer.getSize());

    // Read identifier info
    char identifier[] = {'C', '3', 'B', '\0'};
    char sig[4];
    if (_binaryReader.read(sig, 1, 4) != 4 || memcmp(sig, identifier, 4) != 0)
    {
        clear();
        AXLOG("warning: Invalid identifier: %s", path.data());
        return false;
    }

    // Read version
    unsigned char ver[2];
    if (_binaryReader.read(ver, 1, 2) != 2)
    {
        AXLOG("warning: Failed to read version:");
        return false;
    }

    char version[20] = {0};
    snprintf(version, sizeof(version), "%d.%d", ver[0], ver[1]);
    _version = version;

    // Read ref table size
    if (_binaryReader.read(&_referenceCount, 4, 1) != 1)
    {
        clear();
        AXLOG("warning: Failed to read ref table size '%s'.", path.data());
        return false;
    }

    // Read all refs
    AX_SAFE_DELETE_ARRAY(_references);
    _references = new Reference[_referenceCount];
    for (unsigned int i = 0; i < _referenceCount; ++i)
    {
        if ((_references[i].id = _binaryReader.readString()).empty() ||
            _binaryReader.read(&_references[i].type, 4, 1) != 1 ||
            _binaryReader.read(&_references[i].offset, 4, 1) != 1)
        {
            clear();
            AXLOG("warning: Failed to read ref number %u for bundle '%s'.", i, path.data());
            AX_SAFE_DELETE_ARRAY(_references);
            return false;
        }
    }

    return true;
}

bool Bundle3D::loadMeshDataJson_0_1(MeshDatas& meshdatas)
{
    const rapidjson::Value& mesh_data_array = _jsonReader[MESH];
    MeshData* meshdata                      = new MeshData();
    const rapidjson::Value& mesh_data_val   = mesh_data_array[(rapidjson::SizeType)0];

    const rapidjson::Value& mesh_data_body_array = mesh_data_val[DEFAULTPART];

    const rapidjson::Value& mesh_data_body_array_0 = mesh_data_body_array[(rapidjson::SizeType)0];

    // mesh_vertex_attribute
    const rapidjson::Value& mesh_vertex_attribute = mesh_data_val[ATTRIBUTES];
    meshdata->attribCount                         = mesh_vertex_attribute.Size();
    meshdata->attribs.resize(meshdata->attribCount);
    for (rapidjson::SizeType i = 0; i < mesh_vertex_attribute.Size(); ++i)
    {
        const rapidjson::Value& mesh_vertex_attribute_val = mesh_vertex_attribute[i];

        int size                          = mesh_vertex_attribute_val[ATTRIBUTESIZE].GetUint();
        meshdata->attribs[i].type         = parseGLDataType(mesh_vertex_attribute_val[TYPE].GetString(), size);
        meshdata->attribs[i].vertexAttrib = parseGLProgramAttribute(mesh_vertex_attribute_val[ATTRIBUTE].GetString());
    }

    // vertices
    meshdata->vertexSizeInFloat = mesh_data_body_array_0[VERTEXSIZE].GetInt();
    meshdata->vertex.resize(meshdata->vertexSizeInFloat);

    const rapidjson::Value& mesh_data_body_vertices = mesh_data_body_array_0[VERTICES];
    for (rapidjson::SizeType i = 0; i < mesh_data_body_vertices.Size(); ++i)
        meshdata->vertex[i] = mesh_data_body_vertices[i].GetFloat();

    // index_number
    unsigned int indexnum = mesh_data_body_array_0[INDEXNUM].GetUint();

    // indices
    IndexArray indices{};
    indices.resize(indexnum);

    const rapidjson::Value& indices_val_array = mesh_data_body_array_0[INDICES];
    for (rapidjson::SizeType i = 0; i < indices_val_array.Size(); ++i)
        indices.at<uint16_t>(i) = (unsigned short)indices_val_array[i].GetUint();

    auto& storedIndices = meshdata->subMeshIndices.emplace_back(std::move(indices));
    meshdata->subMeshAABB.emplace_back(calculateAABB(meshdata->vertex, meshdata->getPerVertexSize(), storedIndices));
    meshdatas.meshDatas.emplace_back(meshdata);
    return true;
}

bool Bundle3D::loadMeshDataJson_0_2(MeshDatas& meshdatas)
{
    MeshData* meshdata                   = new MeshData();
    const rapidjson::Value& mesh_array   = _jsonReader[MESH];
    const rapidjson::Value& mesh_array_0 = mesh_array[(rapidjson::SizeType)0];

    // mesh_vertex_attribute
    const rapidjson::Value& mesh_vertex_attribute = mesh_array_0[ATTRIBUTES];
    meshdata->attribCount                         = mesh_vertex_attribute.Size();
    meshdata->attribs.resize(meshdata->attribCount);
    for (rapidjson::SizeType i = 0; i < mesh_vertex_attribute.Size(); ++i)
    {
        const rapidjson::Value& mesh_vertex_attribute_val = mesh_vertex_attribute[i];

        auto size                         = mesh_vertex_attribute_val[ATTRIBUTESIZE].GetUint();
        meshdata->attribs[i].type         = parseGLDataType(mesh_vertex_attribute_val[TYPE].GetString(), size);
        meshdata->attribs[i].vertexAttrib = parseGLProgramAttribute(mesh_vertex_attribute_val[ATTRIBUTE].GetString());
    }

    // vertices
    const rapidjson::Value& mesh_data_vertex   = mesh_array_0[VERTEX];
    const rapidjson::Value& mesh_data_vertex_0 = mesh_data_vertex[(rapidjson::SizeType)0];

    meshdata->vertexSizeInFloat = mesh_data_vertex_0[VERTEXSIZE].GetInt();
    meshdata->vertex.resize(meshdata->vertexSizeInFloat);

    const rapidjson::Value& mesh_data_body_vertices = mesh_data_vertex_0[VERTICES];
    for (rapidjson::SizeType i = 0; i < mesh_data_body_vertices.Size(); ++i)
        meshdata->vertex[i] = mesh_data_body_vertices[i].GetFloat();

    // submesh
    const rapidjson::Value& mesh_submesh_array = mesh_array_0[SUBMESH];
    for (rapidjson::SizeType i = 0; i < mesh_submesh_array.Size(); ++i)
    {
        const rapidjson::Value& mesh_submesh_val = mesh_submesh_array[i];
        // std::string id = mesh_submesh_val[ID].GetString();

        // index_number
        unsigned int indexnum = mesh_submesh_val[INDEXNUM].GetUint();

        // indices
        IndexArray indices{};
        indices.resize(indexnum);

        const rapidjson::Value& indices_val_array = mesh_submesh_val[INDICES];
        for (rapidjson::SizeType j = 0; j < indices_val_array.Size(); ++j)
            indices.at<uint16_t>(j) = (unsigned short)indices_val_array[j].GetUint();

        auto& storedIndices = meshdata->subMeshIndices.emplace_back(std::move(indices));
        meshdata->subMeshAABB.emplace_back(calculateAABB(meshdata->vertex, meshdata->getPerVertexSize(), storedIndices));
    }
    meshdatas.meshDatas.emplace_back(meshdata);
    return true;
}

bool Bundle3D::loadSkinDataJson(SkinData* skindata)
{
    if (!_jsonReader.HasMember(SKIN))
        return false;

    const rapidjson::Value& skin_data_array = _jsonReader[SKIN];

    AXASSERT(skin_data_array.IsArray(), "skin data is not an array");
    const rapidjson::Value& skin_data_array_val_0 = skin_data_array[(rapidjson::SizeType)0];

    if (!skin_data_array_val_0.HasMember(BONES))
        return false;

    const rapidjson::Value& skin_data_bones = skin_data_array_val_0[BONES];
    for (rapidjson::SizeType i = 0; i < skin_data_bones.Size(); ++i)
    {
        const rapidjson::Value& skin_data_bone = skin_data_bones[i];
        std::string name                       = skin_data_bone[NODE].GetString();
        skindata->addSkinBoneNames(name);

        Mat4 mat_bind_pos;
        const rapidjson::Value& bind_pos = skin_data_bone[BINDSHAPE];
        for (rapidjson::SizeType j = 0; j < bind_pos.Size(); ++j)
        {
            mat_bind_pos.m[j] = bind_pos[j].GetFloat();
        }
        skindata->inverseBindPoseMatrices.emplace_back(mat_bind_pos);
    }

    // set root bone information
    const rapidjson::Value& skin_data_1 = skin_data_array[1];

    // parent and child relationship map
    skindata->skinBoneOriginMatrices.resize(skindata->skinBoneNames.size());
    getChildMap(skindata->boneChild, skindata, skin_data_1);
    return true;
}

bool Bundle3D::loadSkinDataBinary(SkinData* skindata)
{
    if (!seekToFirstType(BUNDLE_TYPE_MESHSKIN))
        return false;

    std::string boneName = _binaryReader.readString();

    // transform
    float bindShape[16];
    if (!_binaryReader.readMatrix(bindShape))
    {
        AXLOG("warning: Failed to read SkinData: bindShape matrix  '%s'.", _path.c_str());
        return false;
    }

    // bone count
    unsigned int boneNum;
    if (!_binaryReader.read(&boneNum))
    {
        AXLOG("warning: Failed to read SkinData: boneNum  '%s'.", _path.c_str());
        return false;
    }

    // Fix bug: check if the bone number is 0.
    if (boneNum == 0)
        return false;

    // bone names and bind pos
    float bindpos[16];
    for (unsigned int i = 0; i < boneNum; ++i)
    {
        std::string skinBoneName = _binaryReader.readString();
        skindata->skinBoneNames.emplace_back(skinBoneName);
        if (!_binaryReader.readMatrix(bindpos))
        {
            AXLOG("warning: Failed to load SkinData: bindpos '%s'.", _path.c_str());
            return false;
        }
        skindata->inverseBindPoseMatrices.emplace_back(bindpos);
    }

    skindata->skinBoneOriginMatrices.resize(boneNum);

    boneName = _binaryReader.readString();

    // bind shape
    _binaryReader.readMatrix(bindShape);
    int rootIndex = skindata->getSkinBoneNameIndex(boneName);
    if (rootIndex < 0)
    {
        skindata->addNodeBoneNames(boneName);
        rootIndex = skindata->getBoneNameIndex(boneName);
        skindata->nodeBoneOriginMatrices.emplace_back(bindShape);
    }
    else
    {
        skindata->skinBoneOriginMatrices[rootIndex] = bindShape;
    }

    // set root bone index
    skindata->rootBoneIndex = rootIndex;

    // read parent and child relationship map
    float transform[16];
    unsigned int linkNum;
    _binaryReader.read(&linkNum);
    for (unsigned int i = 0; i < linkNum; ++i)
    {
        std::string id = _binaryReader.readString();
        int index      = skindata->getSkinBoneNameIndex(id);

        std::string parentid = _binaryReader.readString();

        if (!_binaryReader.readMatrix(transform))
        {
            AXLOG("warning: Failed to load SkinData: transform '%s'.", _path.c_str());
            return false;
        }

        if (index < 0)
        {
            skindata->addNodeBoneNames(id);
            index = skindata->getBoneNameIndex(id);
            skindata->nodeBoneOriginMatrices.emplace_back(transform);
        }
        else
        {
            skindata->skinBoneOriginMatrices[index] = transform;
        }

        int parentIndex = skindata->getSkinBoneNameIndex(parentid);
        if (parentIndex < 0)
        {
            skindata->addNodeBoneNames(parentid);
            parentIndex = skindata->getBoneNameIndex(parentid);
        }

        skindata->boneChild[parentIndex].emplace_back(index);
    }

    return true;
}

bool Bundle3D::loadMaterialDataJson_0_1(MaterialDatas& materialdatas)
{
    if (!_jsonReader.HasMember(MATERIAL))
        return false;
    NMaterialData materialData;
    const rapidjson::Value& material_data_array = _jsonReader[MATERIAL];

    if (material_data_array.Size() > 0)
    {
        const rapidjson::Value& material_data_array_0 = material_data_array[(rapidjson::SizeType)0];
        if (material_data_array_0.HasMember(BASE))
        {
            const rapidjson::Value& material_data_base_array   = material_data_array_0[BASE];
            const rapidjson::Value& material_data_base_array_0 = material_data_base_array[(rapidjson::SizeType)0];
            NTextureData textureData;
            // set texture
            std::string filename = material_data_base_array_0[FILENAME].GetString();
            textureData.filename = filename.empty() ? filename : _modelPath + filename;
            textureData.type     = NTextureData::Usage::Diffuse;
            textureData.id       = "";
            materialData.textures.emplace_back(textureData);
            materialdatas.materials.emplace_back(materialData);
        }
    }

    return true;
}

bool Bundle3D::loadMaterialDataJson_0_2(MaterialDatas& materialdatas)
{
    if (!_jsonReader.HasMember(MATERIAL))
        return false;
    NMaterialData materialData;
    const rapidjson::Value& material_array = _jsonReader[MATERIAL];

    for (rapidjson::SizeType i = 0; i < material_array.Size(); ++i)
    {
        NTextureData textureData;
        const rapidjson::Value& material_val = material_array[i];

        // set texture
        std::string filename = material_val[TEXTURES].GetString();
        textureData.filename = filename.empty() ? filename : _modelPath + filename;
        textureData.type     = NTextureData::Usage::Diffuse;
        textureData.id       = "";
        materialData.textures.emplace_back(textureData);
    }
    materialdatas.materials.emplace_back(materialData);
    return true;
}

bool loadMaterialDataJson(MaterialData* /*materialdata*/)
{
    return true;
}

bool loadMaterialDataJson_0_1(MaterialData* /*materialdata*/)
{
    return true;
}

bool loadMaterialDataJson_0_2(MaterialData* /*materialdata*/)
{
    return true;
}

bool Bundle3D::loadAnimationDataJson(std::string_view id, Animation3DData* animationdata)
{
    std::string anim = "";
    if (_version == "1.2" || _version == "0.2")
        anim = ANIMATION;
    else
        anim = ANIMATIONS;

    if (!_jsonReader.HasMember(anim.c_str()))
        return false;
    int the_index                                = -1;
    const rapidjson::Value& animation_data_array = _jsonReader[anim.c_str()];

    if (animation_data_array.Size() == 0)
        return false;

    if (!id.empty())
    {
        for (rapidjson::SizeType i = 0; i < animation_data_array.Size(); ++i)
        {
            if (animation_data_array[i][ID].GetString() == id)
            {
                the_index = static_cast<int>(i);
            }
        }
        if (the_index < 0)
            return false;
    }
    else
    {
        the_index = 0;
    }

    const rapidjson::Value& animation_data_array_val_0 = animation_data_array[(rapidjson::SizeType)the_index];

    animationdata->_totalTime = animation_data_array_val_0[LENGTH].GetFloat();

    const rapidjson::Value& bones = animation_data_array_val_0[BONES];
    for (rapidjson::SizeType i = 0; i < bones.Size(); ++i)
    {
        const rapidjson::Value& bone = bones[i];
        std::string bone_name        = bone[BONEID].GetString();

        if (bone.HasMember(KEYFRAMES))
        {
            const rapidjson::Value& bone_keyframes = bone[KEYFRAMES];
            rapidjson::SizeType keyframe_size      = bone_keyframes.Size();
            animationdata->_rotationKeys[bone_name].reserve(keyframe_size);
            animationdata->_scaleKeys[bone_name].reserve(keyframe_size);
            animationdata->_translationKeys[bone_name].reserve(keyframe_size);

            for (rapidjson::SizeType j = 0; j < keyframe_size; ++j)
            {
                const rapidjson::Value& bone_keyframe = bone_keyframes[j];

                if (bone_keyframe.HasMember(TRANSLATION))
                {
                    const rapidjson::Value& bone_keyframe_translation = bone_keyframe[TRANSLATION];
                    float keytime                                     = bone_keyframe[KEYTIME].GetFloat();
                    Vec3 val(bone_keyframe_translation[(rapidjson::SizeType)0].GetFloat(),
                             bone_keyframe_translation[1].GetFloat(), bone_keyframe_translation[2].GetFloat());
                    animationdata->_translationKeys[bone_name].emplace_back(Animation3DData::Vec3Key(keytime, val));
                }

                if (bone_keyframe.HasMember(ROTATION))
                {
                    const rapidjson::Value& bone_keyframe_rotation = bone_keyframe[ROTATION];
                    float keytime                                  = bone_keyframe[KEYTIME].GetFloat();
                    Quaternion val                                 = Quaternion(
                                                        bone_keyframe_rotation[(rapidjson::SizeType)0].GetFloat(), bone_keyframe_rotation[1].GetFloat(),
                                                        bone_keyframe_rotation[2].GetFloat(), bone_keyframe_rotation[3].GetFloat());
                    animationdata->_rotationKeys[bone_name].emplace_back(Animation3DData::QuatKey(keytime, val));
                }

                if (bone_keyframe.HasMember(SCALE))
                {
                    const rapidjson::Value& bone_keyframe_scale = bone_keyframe[SCALE];
                    float keytime                               = bone_keyframe[KEYTIME].GetFloat();
                    Vec3 val(bone_keyframe_scale[(rapidjson::SizeType)0].GetFloat(), bone_keyframe_scale[1].GetFloat(),
                             bone_keyframe_scale[2].GetFloat());
                    animationdata->_scaleKeys[bone_name].emplace_back(Animation3DData::Vec3Key(keytime, val));
                }
            }
        }
    }

    return true;
}

bool Bundle3D::loadAnimationDataBinary(std::string_view id, Animation3DData* animationdata)
{

    if (_version == "0.1" || _version == "0.2" || _version == "0.3" || _version == "0.4")
    {
        if (!seekToFirstType(BUNDLE_TYPE_ANIMATIONS))
            return false;
    }
    else
    {
        // if id is not a null string, we need to add a suffix of "animation" for seeding.
        std::string id_{id};
        if (!id.empty())
            id_.append("animation");

        if (!seekToFirstType(BUNDLE_TYPE_ANIMATIONS, id_))
            return false;
    }

    unsigned int animNum = 1;
    if (_version == "0.3" || _version == "0.4")
    {
        if (!_binaryReader.read(&animNum))
        {
            AXLOG("warning: Failed to read AnimationData: animNum '%s'.", _path.c_str());
            return false;
        }
    }

    bool has_found = false;
    for (unsigned int k = 0; k < animNum; ++k)
    {
        animationdata->resetData();
        std::string animId = _binaryReader.readString();

        if (!_binaryReader.read(&animationdata->_totalTime))
        {
            AXLOG("warning: Failed to read AnimationData: totalTime '%s'.", _path.c_str());
            return false;
        }

        unsigned int nodeAnimationNum;
        if (!_binaryReader.read(&nodeAnimationNum))
        {
            AXLOG("warning: Failed to read AnimationData: animNum '%s'.", _path.c_str());
            return false;
        }
        for (unsigned int i = 0; i < nodeAnimationNum; ++i)
        {
            std::string boneName = _binaryReader.readString();
            unsigned int keyframeNum;
            if (!_binaryReader.read(&keyframeNum))
            {
                AXLOG("warning: Failed to read AnimationData: keyframeNum '%s'.", _path.c_str());
                return false;
            }

            animationdata->_rotationKeys[boneName].reserve(keyframeNum);
            animationdata->_scaleKeys[boneName].reserve(keyframeNum);
            animationdata->_translationKeys[boneName].reserve(keyframeNum);

            for (unsigned int j = 0; j < keyframeNum; ++j)
            {
                float keytime;
                if (!_binaryReader.read(&keytime))
                {
                    AXLOG("warning: Failed to read AnimationData: keytime '%s'.", _path.c_str());
                    return false;
                }

                // transform flag
                unsigned char transformFlag(0);
                if (_version != "0.1" && _version != "0.2" && _version != "0.3")
                {
                    if (!_binaryReader.read(&transformFlag))
                    {
                        AXLOG("warning: Failed to read AnimationData: transformFlag '%s'.", _path.c_str());
                        return false;
                    }
                }

                // rotation
                bool hasRotate = true;
                if (_version != "0.1" && _version != "0.2" && _version != "0.3")
                    hasRotate = transformFlag & 0x01;

                if (hasRotate)
                {
                    Quaternion rotate;
                    if (_binaryReader.read(&rotate, 4, 4) != 4)
                    {
                        AXLOG("warning: Failed to read AnimationData: rotate '%s'.", _path.c_str());
                        return false;
                    }
                    animationdata->_rotationKeys[boneName].emplace_back(Animation3DData::QuatKey(keytime, rotate));
                }

                // scale
                bool hasScale = true;
                if (_version != "0.1" && _version != "0.2" && _version != "0.3")
                    hasScale = (transformFlag >> 1) & 0x01;

                if (hasScale)
                {
                    Vec3 scale;
                    if (_binaryReader.read(&scale, 4, 3) != 3)
                    {
                        AXLOG("warning: Failed to read AnimationData: scale '%s'.", _path.c_str());
                        return false;
                    }
                    animationdata->_scaleKeys[boneName].emplace_back(Animation3DData::Vec3Key(keytime, scale));
                }

                // translation
                bool hasTranslation = true;
                if (_version != "0.1" && _version != "0.2" && _version != "0.3")
                    hasTranslation = (transformFlag >> 2) & 0x01;

                if (hasTranslation)
                {
                    Vec3 position;
                    if (_binaryReader.read(&position, 4, 3) != 3)
                    {
                        AXLOG("warning: Failed to read AnimationData: position '%s'.", _path.c_str());
                        return false;
                    }
                    animationdata->_translationKeys[boneName].emplace_back(Animation3DData::Vec3Key(keytime, position));
                }
            }
        }
        if (id == animId || id.empty())
        {
            has_found = true;
            break;
        }
    }
    if (!has_found)
    {
        animationdata->resetData();
        return false;
    }
    return true;
}

bool Bundle3D::loadNodesJson(NodeDatas& nodedatas)
{
    if (!_jsonReader.HasMember(NODES))
        return false;
    const rapidjson::Value& nodes = _jsonReader[NODES];
    if (!nodes.IsArray())
        return false;

    // traverse the nodes again
    for (rapidjson::SizeType i = 0; i < nodes.Size(); ++i)
    {
        const rapidjson::Value& jnode = nodes[i];
        std::string id                = jnode[ID].GetString();
        NodeData* nodedata            = parseNodesRecursivelyJson(jnode, nodes.Size() == 1);

        bool isSkeleton = jnode[SKELETON].GetBool();
        if (isSkeleton)
            nodedatas.skeleton.emplace_back(nodedata);
        else
            nodedatas.nodes.emplace_back(nodedata);
    }
    return true;
}
NodeData* Bundle3D::parseNodesRecursivelyJson(const rapidjson::Value& jvalue, bool singleSprite)
{
    NodeData* nodedata = new NodeData();
    // id
    nodedata->id = jvalue[ID].GetString();

    // transform
    Mat4 transform;
    const rapidjson::Value& jtransform = jvalue[TRANSFORM];

    for (rapidjson::SizeType j = 0; j < jtransform.Size(); ++j)
    {
        transform.m[j] = jtransform[j].GetFloat();
    }

    nodedata->transform = transform;

    bool isSkin = false;

    // parts
    if (jvalue.HasMember(PARTS))
    {
        const rapidjson::Value& parts = jvalue[PARTS];

        for (rapidjson::SizeType i = 0; i < parts.Size(); ++i)
        {
            auto modelnodedata           = new ModelData();
            const rapidjson::Value& part = parts[i];
            modelnodedata->subMeshId     = part[MESHPARTID].GetString();
            modelnodedata->materialId    = part[MATERIALID].GetString();

            if (modelnodedata->subMeshId == "" || modelnodedata->materialId == "")
            {
                AXLOG("warning: Node %s part is missing meshPartId or materialId", nodedata->id.c_str());
                AX_SAFE_DELETE(modelnodedata);
                AX_SAFE_DELETE(nodedata);
                return nullptr;
            }

            if (part.HasMember(BONES))
            {
                const rapidjson::Value& bones = part[BONES];

                for (rapidjson::SizeType j = 0; j < bones.Size(); ++j)
                {
                    const rapidjson::Value& bone = bones[j];

                    // node
                    if (!bone.HasMember(NODE))
                    {
                        AXLOG("warning: Bone node ID missing");
                        AX_SAFE_DELETE(modelnodedata);
                        AX_SAFE_DELETE(nodedata);
                        return nullptr;
                    }

                    modelnodedata->bones.emplace_back(bone[NODE].GetString());

                    Mat4 invbindpos;
                    const rapidjson::Value& jinvbindpos = bone[TRANSFORM];

                    for (rapidjson::SizeType k = 0; k < jinvbindpos.Size(); ++k)
                    {
                        invbindpos.m[k] = jinvbindpos[k].GetFloat();
                    }

                    // invbindpos.inverse();
                    modelnodedata->invBindPose.emplace_back(invbindpos);
                }

                if (bones.Size() > 0)
                    isSkin = true;
            }
            nodedata->modelNodeDatas.emplace_back(modelnodedata);
        }
    }

    // set transform
    if (_version == "0.1" || _version == "0.2" || _version == "0.3" || _version == "0.4" || _version == "0.5" ||
        _version == "0.6")
    {
        if (isSkin || singleSprite)
        {
            nodedata->transform = Mat4::IDENTITY;
        }
        else
        {
            nodedata->transform = transform;
        }
    }
    else
    {
        nodedata->transform = transform;
    }

    if (jvalue.HasMember(CHILDREN))
    {
        const rapidjson::Value& children = jvalue[CHILDREN];
        for (rapidjson::SizeType i = 0; i < children.Size(); ++i)
        {
            const rapidjson::Value& child = children[i];

            NodeData* tempdata = parseNodesRecursivelyJson(child, singleSprite);
            nodedata->children.emplace_back(tempdata);
        }
    }
    return nodedata;
}

bool Bundle3D::loadNodesBinary(NodeDatas& nodedatas)
{
    if (!seekToFirstType(BUNDLE_TYPE_NODE))
        return false;

    unsigned int nodeSize = 0;
    if (_binaryReader.read(&nodeSize, 4, 1) != 1)
    {
        AXLOG("warning: Failed to read nodes");
        return false;
    }

    // traverse the nodes again
    for (rapidjson::SizeType i = 0; i < nodeSize; ++i)
    {
        bool skeleton      = false;
        NodeData* nodedata = parseNodesRecursivelyBinary(skeleton, nodeSize == 1);

        if (skeleton)
            nodedatas.skeleton.emplace_back(nodedata);
        else
            nodedatas.nodes.emplace_back(nodedata);
    }
    return true;
}
NodeData* Bundle3D::parseNodesRecursivelyBinary(bool& skeleton, bool singleSprite)
{
    // id
    std::string id = _binaryReader.readString();
    // is skeleton
    bool skeleton_;
    if (_binaryReader.read(&skeleton_, 1, 1) != 1)
    {
        AXLOG("warning: Failed to read is skeleton");
        return nullptr;
    }
    if (skeleton_)
        skeleton = true;

    // transform
    Mat4 transform;
    if (!_binaryReader.readMatrix(transform.m))
    {
        AXLOG("warning: Failed to read transform matrix");
        return nullptr;
    }
    // parts
    unsigned int partsSize = 0;
    if (_binaryReader.read(&partsSize, 4, 1) != 1)
    {
        AXLOG("warning: Failed to read meshdata: attribCount '%s'.", _path.c_str());
        return nullptr;
    }

    NodeData* nodedata = new NodeData();
    nodedata->id       = id;

    bool isSkin = false;

    if (partsSize > 0)
    {
        for (unsigned int i = 0; i < partsSize; ++i)
        {
            auto modelnodedata        = new ModelData();
            modelnodedata->subMeshId  = _binaryReader.readString();
            modelnodedata->materialId = _binaryReader.readString();

            if (modelnodedata->subMeshId.empty() || modelnodedata->materialId.empty())
            {
                AXLOG("Node %s part is missing meshPartId or materialId", nodedata->id.c_str());
                AX_SAFE_DELETE(modelnodedata);
                AX_SAFE_DELETE(nodedata);
                return nullptr;
            }

            // read bone
            unsigned int bonesSize = 0;
            if (_binaryReader.read(&bonesSize, 4, 1) != 1)
            {
                AXLOG("warning: Failed to read meshdata: attribCount '%s'.", _path.c_str());
                AX_SAFE_DELETE(modelnodedata);
                AX_SAFE_DELETE(nodedata);
                return nullptr;
            }

            if (bonesSize > 0)
            {
                for (unsigned int j = 0; j < bonesSize; ++j)
                {
                    std::string name = _binaryReader.readString();
                    modelnodedata->bones.emplace_back(name);

                    Mat4 invbindpos;
                    if (!_binaryReader.readMatrix(invbindpos.m))
                    {
                        AX_SAFE_DELETE(modelnodedata);
                        AX_SAFE_DELETE(nodedata);
                        return nullptr;
                    }

                    modelnodedata->invBindPose.emplace_back(invbindpos);
                }
                isSkin = true;
            }
            unsigned int uvMapping = 0;
            if (_binaryReader.read(&uvMapping, 4, 1) != 1)
            {
                AXLOG("warning: Failed to read nodedata: uvMapping '%s'.", _path.c_str());
                AX_SAFE_DELETE(modelnodedata);
                AX_SAFE_DELETE(nodedata);
                return nullptr;
            }
            for (unsigned int j = 0; j < uvMapping; ++j)
            {
                unsigned int textureIndexSize = 0;
                if (_binaryReader.read(&textureIndexSize, 4, 1) != 1)
                {
                    AXLOG("warning: Failed to read meshdata: attribCount '%s'.", _path.c_str());
                    AX_SAFE_DELETE(modelnodedata);
                    AX_SAFE_DELETE(nodedata);
                    return nullptr;
                }
                for (unsigned int k = 0; k < textureIndexSize; ++k)
                {
                    unsigned int index = 0;
                    if (_binaryReader.read(&index, 4, 1) != 1)
                    {
                        AX_SAFE_DELETE(modelnodedata);
                        AX_SAFE_DELETE(nodedata);
                        return nullptr;
                    }
                }
            }
            nodedata->modelNodeDatas.emplace_back(modelnodedata);
        }
    }

    // set transform
    if (_version == "0.1" || _version == "0.2" || _version == "0.3" || _version == "0.4" || _version == "0.5" ||
        _version == "0.6")
    {
        if (isSkin || singleSprite)
        {
            nodedata->transform = Mat4::IDENTITY;
        }
        else
        {
            nodedata->transform = transform;
        }
    }
    else
    {
        nodedata->transform = transform;
    }

    unsigned int childrenSize = 0;
    if (_binaryReader.read(&childrenSize, 4, 1) != 1)
    {
        AXLOG("warning: Failed to read meshdata: attribCount '%s'.", _path.c_str());
        AX_SAFE_DELETE(nodedata);
        return nullptr;
    }
    if (childrenSize > 0)
    {
        for (unsigned int i = 0; i < childrenSize; ++i)
        {
            NodeData* tempdata = parseNodesRecursivelyBinary(skeleton, singleSprite);
            nodedata->children.emplace_back(tempdata);
        }
    }
    return nodedata;
}

backend::VertexFormat Bundle3D::parseGLDataType(std::string_view str, int size)
{
    backend::VertexFormat ret = backend::VertexFormat::INT;
    if (str == "GL_BYTE")
    {
        switch (size)
        {
        case 4:
            return backend::VertexFormat::UBYTE4;
        default:
            AXLOGERROR("parseVertexType GL_BYTE x %d error", size);
        }
    }
    else if (str == "GL_UNSIGNED_BYTE")
    {
        switch (size)
        {
        case 4:
            return backend::VertexFormat::UBYTE4;
        default:
            AXLOGERROR("parseVertexType GL_UNSIGNED_BYTE x %d error", size);
        }
    }
    else if (str == "GL_SHORT")
    {
        switch (size)
        {
        case 2:
            return backend::VertexFormat::USHORT2;
        case 4:
            return backend::VertexFormat::USHORT4;
        default:
            AXLOGERROR("parseVertexType GL_SHORT x %d error", size);
        }
    }
    else if (str == "GL_UNSIGNED_SHORT")
    {
        switch (size)
        {
        case 2:
            return backend::VertexFormat::USHORT2;
        case 4:
            return backend::VertexFormat::USHORT4;
        default:
            AXLOGERROR("parseVertexType GL_UNSIGNED_SHORT x %d error", size);
        }
    }
    else if (str == "GL_INT")
    {
        switch (size)
        {
        case 1:
            return backend::VertexFormat::INT;
        case 2:
            return backend::VertexFormat::INT2;
        case 3:
            return backend::VertexFormat::INT3;
        case 4:
            return backend::VertexFormat::INT4;
        default:
            AXLOGERROR("parseVertexType GL_INT x %d error", size);
        }
    }
    else if (str == "GL_UNSIGNED_INT")
    {
        switch (size)
        {
        case 1:
            return backend::VertexFormat::INT;
        case 2:
            return backend::VertexFormat::INT2;
        case 3:
            return backend::VertexFormat::INT3;
        case 4:
            return backend::VertexFormat::INT4;
        default:
            AXLOGERROR("parseVertexType GL_UNSIGNED_INT x %d error", size);
        }
    }
    else if (str == "GL_FLOAT")
    {
        switch (size)
        {
        case 1:
            return backend::VertexFormat::FLOAT;
        case 2:
            return backend::VertexFormat::FLOAT2;
        case 3:
            return backend::VertexFormat::FLOAT3;
        case 4:
            return backend::VertexFormat::FLOAT4;
        default:
            AXLOGERROR("parseVertexType GL_UNSIGNED_INT x %d error", size);
        }
    }
    AXASSERT(false, "parseVertexType failed!");
    return ret;
}

backend::SamplerAddressMode Bundle3D::parseSamplerAddressMode(std::string_view str)
{

    if (str == "REPEAT")
    {
        return backend::SamplerAddressMode::REPEAT;
    }
    else if (str == "CLAMP")
    {
        return backend::SamplerAddressMode::CLAMP_TO_EDGE;
    }
    else
    {
        AXASSERT(false, "Invalid GL type");
        return backend::SamplerAddressMode::REPEAT;
    }
}

NTextureData::Usage Bundle3D::parseGLTextureType(std::string_view str)
{
    if (str == "AMBIENT")
    {
        return NTextureData::Usage::Ambient;
    }
    else if (str == "BUMP")
    {
        return NTextureData::Usage::Bump;
    }
    else if (str == "DIFFUSE")
    {
        return NTextureData::Usage::Diffuse;
    }
    else if (str == "EMISSIVE")
    {
        return NTextureData::Usage::Emissive;
    }
    else if (str == "NONE")
    {
        return NTextureData::Usage::None;
    }
    else if (str == "NORMAL")
    {
        return NTextureData::Usage::Normal;
    }
    else if (str == "REFLECTION")
    {
        return NTextureData::Usage::Reflection;
    }
    else if (str == "SHININESS")
    {
        return NTextureData::Usage::Shininess;
    }
    else if (str == "SPECULAR")
    {
        return NTextureData::Usage::Specular;
    }
    else if (str == "TRANSPARENCY")
    {
        return NTextureData::Usage::Transparency;
    }
    else
    {
        AXASSERT(false, "Wrong Texture type");
        return NTextureData::Usage::Unknown;
    }
}
shaderinfos::VertexKey Bundle3D::parseGLProgramAttribute(std::string_view str)
{
    if (str == "VERTEX_ATTRIB_POSITION")
    {
        return shaderinfos::VertexKey::VERTEX_ATTRIB_POSITION;
    }
    else if (str == "VERTEX_ATTRIB_COLOR")
    {
        return shaderinfos::VertexKey::VERTEX_ATTRIB_COLOR;
    }
    else if (str == "VERTEX_ATTRIB_TEX_COORD")
    {
        return shaderinfos::VertexKey::VERTEX_ATTRIB_TEX_COORD;
    }
    else if (str == "VERTEX_ATTRIB_TEX_COORD1")
    {
        return shaderinfos::VertexKey::VERTEX_ATTRIB_TEX_COORD1;
    }
    else if (str == "VERTEX_ATTRIB_TEX_COORD2")
    {
        return shaderinfos::VertexKey::VERTEX_ATTRIB_TEX_COORD2;
    }
    else if (str == "VERTEX_ATTRIB_TEX_COORD3")
    {
        return shaderinfos::VertexKey::VERTEX_ATTRIB_TEX_COORD3;
    }
    // comment out them
    //    else if (str == "VERTEX_ATTRIB_TEX_COORD4")
    //    {
    //        return shaderinfos::VertexKey::VERTEX_ATTRIB_TEX_COORD4;
    //    }
    //    else if (str == "VERTEX_ATTRIB_TEX_COORD5")
    //    {
    //        return shaderinfos::VertexKey::VERTEX_ATTRIB_TEX_COORD5;
    //    }
    //    else if (str == "VERTEX_ATTRIB_TEX_COORD6")
    //    {
    //        return shaderinfos::VertexKey::VERTEX_ATTRIB_TEX_COORD6;
    //    }
    //    else if (str == "VERTEX_ATTRIB_TEX_COORD7")
    //    {
    //        return shaderinfos::VertexKey::VERTEX_ATTRIB_TEX_COORD7;
    //    }
    else if (str == "VERTEX_ATTRIB_NORMAL")
    {
        return shaderinfos::VertexKey::VERTEX_ATTRIB_NORMAL;
    }
    else if (str == "VERTEX_ATTRIB_BLEND_WEIGHT")
    {
        return shaderinfos::VertexKey::VERTEX_ATTRIB_BLEND_WEIGHT;
    }
    else if (str == "VERTEX_ATTRIB_BLEND_INDEX")
    {
        return shaderinfos::VertexKey::VERTEX_ATTRIB_BLEND_INDEX;
    }
    else if (str == "VERTEX_ATTRIB_TANGENT")
    {
        return shaderinfos::VertexKey::VERTEX_ATTRIB_TANGENT;
    }
    else if (str == "VERTEX_ATTRIB_BINORMAL")
    {
        return shaderinfos::VertexKey::VERTEX_ATTRIB_BINORMAL;
    }
    else
    {
        AXASSERT(false, "Wrong Attribute type");
        return shaderinfos::VertexKey::VERTEX_ATTRIB_ERROR;
    }
}

void Bundle3D::getModelRelativePath(std::string_view path)
{
    ssize_t index = path.find_last_of('/');
    std::string fullModelPath;
    _modelPath = path.substr(0, index + 1);
}

Reference* Bundle3D::seekToFirstType(unsigned int type, std::string_view id)
{
    // for each Reference
    for (unsigned int i = 0; i < _referenceCount; ++i)
    {
        Reference* ref = &_references[i];
        if (ref->type == type)
        {
            // if id is not a null string, we also need to check the Reference's id.
            if (id != "" && id != ref->id)
            {
                continue;
            }

            // Found a match
            if (_binaryReader.seek(ref->offset, SEEK_SET) == false)
            {
                AXLOG("warning: Failed to seek to object '%s' in bundle '%s'.", ref->id.c_str(), _path.c_str());
                return nullptr;
            }
            return ref;
        }
    }
    return nullptr;
}

std::vector<Vec3> Bundle3D::getTrianglesList(std::string_view path)
{
    std::vector<Vec3> trianglesList;

    if (path.length() <= 4)
        return trianglesList;

    auto bundle     = Bundle3D::createBundle();
    std::string ext = FileUtils::getInstance()->getFileExtension(path);
    MeshDatas meshs;
    if (ext == ".obj")
    {
        MaterialDatas materials;
        NodeDatas nodes;
        if (!Bundle3D::loadObj(meshs, materials, nodes, path))
        {
            Bundle3D::destroyBundle(bundle);
            return trianglesList;
        }
    }
    else
    {
        if (!bundle->load(path))
        {
            Bundle3D::destroyBundle(bundle);
            return trianglesList;
        }

        bundle->loadMeshDatas(meshs);
    }

    Bundle3D::destroyBundle(bundle);
    for (auto&& iter : meshs.meshDatas)
    {
        int preVertexSize = iter->getPerVertexSize() / sizeof(float);
        for (const auto& indices : iter->subMeshIndices)
        {
            indices.for_each([&](unsigned int ind) {
                trianglesList.emplace_back(Vec3(iter->vertex[ind * preVertexSize], iter->vertex[ind * preVertexSize + 1],
                                             iter->vertex[ind * preVertexSize + 2]));
            });
        }
    }

    return trianglesList;
}

Bundle3D::Bundle3D()
    : _modelPath(""), _path(""), _version(""), _referenceCount(0), _references(nullptr), _isBinary(false)
{}
Bundle3D::~Bundle3D()
{
    clear();
}

ax::AABB Bundle3D::calculateAABB(const std::vector<float>& vertex,
                                      int stride,
                                      const IndexArray& indices)
{
    AABB aabb;
    stride /= 4;

    indices.for_each ([&](uint32_t i) {
        Vec3 point(vertex[i * stride], vertex[i * stride + 1], vertex[i * stride + 2]);
        aabb.updateMinMax(&point, 1); 
    });

    return aabb;
}

NS_AX_END
