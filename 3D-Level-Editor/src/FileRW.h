#include <fstream>
#include <iostream>
#include "json/json.h"
#include <cassert>
#include <errno.h>
#include "String.h"


using namespace std;

#pragma region 定义实体输出文件属性
struct EntityAttribue
{
    string EntityName;
    string mesh;
    string SceneNode;
    double PosX;
    double PosY;
    double PosZ;
    double RotationX;
    double RotationY;
    double RotationZ;
    double ScaleX;
    double ScaleY;
    double ScaleZ;
};
#pragma endregion

static void write_json(string fName, std::vector<EntityAttribue*>& entityVec)
{
   
    Json::Value root;
    Json::Value EntityList;
    Json::FastWriter writer;
    Json::Value Entity;

    for (int i = 0; i < entityVec.size(); i++)
    {
        Entity.clear();
        Entity["EntityName"] = entityVec[i]->EntityName;
        Entity["mesh"] = entityVec[i]->mesh;
        Entity["SceneNode"] = entityVec[i]->SceneNode;
        Entity["PosX"] = entityVec[i]->PosX;
        Entity["PosY"] = entityVec[i]->PosY;
        Entity["PosZ"] = entityVec[i]->PosZ;
        Entity["RotationX"] = entityVec[i]->RotationX;
        Entity["RotationY"] = entityVec[i]->RotationY;
        Entity["RotationZ"] = entityVec[i]->RotationZ;
        Entity["ScaleX"] = entityVec[i]->ScaleX;
        Entity["ScaleY"] = entityVec[i]->ScaleY;
        Entity["ScaleZ"] = entityVec[i]->ScaleZ;
        root.append(Entity);
    }

    string json_file = writer.write(root);
    ofstream ofs;
    string json_filename = fName + ".json";
    ofs.open(json_filename);
    assert(ofs.is_open());
    ofs << json_file;
    cout << root.toStyledString() << endl;

    return;
}

static int read_json(string f, std::vector<EntityAttribue*>& entityVec)
{
    ifstream ifs;
    ifs.open(f + ".json");
    assert(ifs.is_open());
    if (!ifs.is_open()) {
        cout << "Could not open file!" << endl;
        return -1;
    }

    Json::Reader reader;
    Json::Value root;

    if (!reader.parse(ifs, root, false))
    {
        cout << "reader parse error:" << "" << endl;
        return -1;
    }

    int size;
    size = root.size();
    cout << "total " << size << " elements" << endl;
    for (int i = 0; i < size; ++i)
    {
        EntityAttribue* EA = new EntityAttribue();
        EA->EntityName = root[i]["EntityName"].asString();
        EA->mesh = root[i]["mesh"].asString();
        EA->SceneNode = root[i]["SceneNode"].asString();
        EA->PosX = root[i]["PosX"].asDouble();
        EA->PosY = root[i]["PosY"].asDouble();
        EA->PosZ = root[i]["PosZ"].asDouble();
        EA->RotationX = root[i]["RotationX"].asDouble();
        EA->RotationY = root[i]["RotationY"].asDouble();
        EA->RotationZ = root[i]["RotationZ"].asDouble();
        EA->ScaleX = root[i]["ScaleX"].asDouble();
        EA->ScaleY = root[i]["ScaleY"].asDouble();
        EA->ScaleZ = root[i]["ScaleZ"].asDouble();
        entityVec.push_back(EA);
        cout << "EntityName:" << EA->EntityName << ", mesh:" << EA->mesh << endl;
    }
    return 0;
}