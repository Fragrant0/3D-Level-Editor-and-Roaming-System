#include "OIS\OIS.h"
#include "ogre\SdkTrays.h"
#include "ogre\Ogre.h"
#include "FileRW.h"
#include <ogre\OgreUTFString.h>
#include <deque>
#include <set>
#include <vector>
#include <map>

using namespace OgreBites;
using namespace Ogre;
using namespace std;

enum InteractionMode
{
    MODE_SELECT,
    MODE_PLACE_WAYPOINT,
    MODE_CLICK_WALK
};

enum QueryFlags
{
    QUERY_MASK_USER_MODEL = 1 << 0
};

class SelectionRectangle : public Ogre::ManualObject
{
public:
    SelectionRectangle(const Ogre::String& name) : Ogre::ManualObject(name)
    {
        setRenderQueueGroup(Ogre::RENDER_QUEUE_OVERLAY);
        setUseIdentityProjection(true);
        setUseIdentityView(true);
        setQueryFlags(0);
    }

    void setCorners(float left, float top, float right, float bottom)
    {
        left = left * 2.0f - 1.0f;
        right = right * 2.0f - 1.0f;
        top = 1.0f - top * 2.0f;
        bottom = 1.0f - bottom * 2.0f;

        clear();
        begin("", Ogre::RenderOperation::OT_LINE_STRIP);
        position(left, top, -1);
        position(right, top, -1);
        position(right, bottom, -1);
        position(left, bottom, -1);
        position(left, top, -1);
        end();

        setBoundingBox(Ogre::AxisAlignedBox::BOX_INFINITE);
    }
};

class WaypointMarker
{
public:
    WaypointMarker(SceneManager* mgr, const Vector3& pos)
        : mSceneMgr(mgr)
    {
        static int count = 0;
        String name = "WaypointMarker" + StringConverter::toString(count++);
        mEntity = mgr->createEntity(name, "sphere.mesh");
        mEntity->setMaterialName("Examples/Red");
        mEntity->setQueryFlags(0);
        mNode = mgr->getRootSceneNode()->createChildSceneNode(name + "Node", pos);
        mNode->attachObject(mEntity);
        mNode->setScale(0.05f, 0.05f, 0.05f);
    }

    ~WaypointMarker()
    {
        if (mNode)
        {
            mNode->detachAllObjects();
            mNode->getCreator()->destroySceneNode(mNode);
        }
        if (mEntity)
            mSceneMgr->destroyEntity(mEntity);
    }

    void setPosition(const Vector3& pos) { mNode->setPosition(pos); }
    Vector3 getPosition() const { return mNode->getPosition(); }

private:
    SceneManager* mSceneMgr;
    Entity* mEntity;
    SceneNode* mNode;
};

class AppFrameListener : public Ogre::FrameListener, public OIS::MouseListener, public OIS::KeyListener, public OgreBites::SdkTrayListener
{
public:
    AppFrameListener(RenderWindow* pWin, Ogre::Camera* pCam, Ogre::SceneManager* pSceneMag)
    {
        m_pScenemanage = pSceneMag;
        m_pModelsNode = m_pScenemanage->getRootSceneNode()->createChildSceneNode("ModelsNode");

        mRaySceneQuery = m_pScenemanage->createRayQuery(Ogre::Ray());

        mVolQuery = m_pScenemanage->createPlaneBoundedVolumeQuery(Ogre::PlaneBoundedVolumeList());
        mRect = new SelectionRectangle("SelectionRect");
        m_pScenemanage->getRootSceneNode()->createChildSceneNode()->attachObject(mRect);
        mRect->setVisible(false);
        m_bLMouseDown = false;
        m_bDragging = false;

        m_pCurrentAnimState = nullptr;
        m_CurrentAnimIndex = 0;
        m_AnimSpeed = 0.0f;

        size_t windowHnd = 0;
        std::stringstream windowHndStr;
        pWin->getCustomAttribute("WINDOW", &windowHnd);
        windowHndStr << windowHnd;
        OIS::ParamList pl;
        pl.insert(std::make_pair(std::string("WINDOW"), windowHndStr.str()));
        _man = OIS::InputManager::createInputSystem(pl);
        _key = static_cast<OIS::Keyboard*>(_man->createInputObject(OIS::OISKeyboard, true));
        _mouse = static_cast<OIS::Mouse*>(_man->createInputObject(OIS::OISMouse, true));
        _mouse->setEventCallback(this);
        _key->setEventCallback(this);

        mTrayMgr = new SdkTrayManager("SampleControls", pWin, _mouse, this);
        const OIS::MouseState& ms = _mouse->getMouseState();
        ms.height = pWin->getHeight();
        ms.width = pWin->getWidth();

        m_pCam = pCam;
        m_moveSpeed = 50.0f;
        _timer.reset();
        _PolyMode = Ogre::PolygonMode::PM_SOLID;

        m_bContinue = true;
        RMouseButton = false;
        m_CurrentName = "robot";
        m_pCurrentNode = NULL;
        ModelNums = 0;

        m_CurrentMode = MODE_SELECT;
        m_WalkSpeed = 55.0f;
        m_Direction = Vector3::ZERO;
        m_Distance = 0.0f;
        m_Destination = Vector3::ZERO;
        m_WalkingNode = nullptr;
        m_WalkingEntity = nullptr;
        m_WalkAnimState = nullptr;

        m_CompositorEnabled = false;
        m_pViewport = nullptr;

        setupUI();
    }

    ~AppFrameListener()
    {
        if (_man)
        {
            m_pScenemanage->destroyQuery(mRaySceneQuery);
            m_pScenemanage->destroyQuery(mVolQuery);
            delete mRect;

            for (auto* marker : m_WaypointMarkers)
                delete marker;

            _man->destroyInputObject(_key);
            _man->destroyInputObject(_mouse);
            OIS::InputManager::destroyInputSystem(_man);
        }
    }

    Real getTerrainHeight(Real x, Real z)
    {
        Ray ray(Vector3(x, 5000.0f, z), Vector3::NEGATIVE_UNIT_Y);
        mRaySceneQuery->setRay(ray);
        mRaySceneQuery->setSortByDistance(false);
        RaySceneQueryResult& result = mRaySceneQuery->execute();
        for (auto& entry : result)
        {
            if (entry.worldFragment)
                return entry.worldFragment->singleIntersection.y;
        }
        return 0.0f;
    }

    Vector3 getTerrainIntersection(float screenX, float screenY)
    {
        Ray mouseRay = m_pCam->getCameraToViewportRay(screenX, screenY);
        mRaySceneQuery->setRay(mouseRay);
        mRaySceneQuery->setSortByDistance(true);
        mRaySceneQuery->setQueryMask(0xFFFFFFFF);
        RaySceneQueryResult& result = mRaySceneQuery->execute();
        for (auto& entry : result)
        {
            if (entry.worldFragment)
                return entry.worldFragment->singleIntersection;
        }
        return Vector3::ZERO;
    }

    void setViewport(Ogre::Viewport* vp)
    {
        m_pViewport = vp;
    }

    bool hasAnimationState(Ogre::Entity* ent, const Ogre::String& animName)
    {
        if (!ent) return false;
        Ogre::AnimationStateSet* animSet = ent->getAllAnimationStates();
        if (!animSet) return false;
        return animSet->hasAnimationState(animName);
    }

    Ogre::AnimationState* safeGetAnimationState(Ogre::Entity* ent, const Ogre::String& animName)
    {
        if (!ent) return nullptr;
        if (!hasAnimationState(ent, animName)) return nullptr;
        return ent->getAnimationState(animName);
    }

    virtual bool mouseMoved(const OIS::MouseEvent& arg)
    {
        if (mTrayMgr->injectMouseMove(arg)) return true;

        Ogre::Vector3 translate3(0.0, 0.0, 0.0f);
        translate3.z += arg.state.Z.rel * -0.1;
        m_pCam->moveRelative(translate3);

        if (RMouseButton)
        {
            float rotX = arg.state.X.rel * -0.001;
            float rotY = arg.state.Y.rel * -0.001;
            m_pCam->yaw(Ogre::Radian(rotX));
            m_pCam->pitch(Ogre::Radian(rotY));
        }

        float x = (float)arg.state.X.abs / arg.state.width;
        float y = (float)arg.state.Y.abs / arg.state.height;

        if (m_bDragging && !m_SelectedNodes.empty())
        {
            Ogre::Ray mouseRay = m_pCam->getCameraToViewportRay(x, y);
            Vector3 currentGroundPoint = getTerrainIntersection(x, y);

            if (currentGroundPoint != Vector3::ZERO)
            {
                std::map<Ogre::SceneNode*, Ogre::Vector3>::iterator it;
                for (it = m_DragOffsets.begin(); it != m_DragOffsets.end(); ++it)
                {
                    Ogre::SceneNode* node = it->first;
                    Ogre::Vector3 offset = it->second;
                    if (node) {
                        Vector3 newPos = currentGroundPoint + offset;
                        newPos.y = getTerrainHeight(newPos.x, newPos.z);
                        node->setPosition(newPos);
                    }
                }
            }
        }
        else if (m_bLMouseDown) {
            m_MouseStop.x = x;
            m_MouseStop.y = y;
            mRect->setCorners(m_MouseStart.x, m_MouseStart.y, m_MouseStop.x, m_MouseStop.y);
        }

        return true;
    }

    virtual bool mousePressed(const OIS::MouseEvent& arg, OIS::MouseButtonID id)
    {
        if (mTrayMgr->injectMouseDown(arg, id)) return true;

        if (id == OIS::MB_Left) {
            float x = (float)arg.state.X.abs / arg.state.width;
            float y = (float)arg.state.Y.abs / arg.state.height;

            if (m_CurrentMode == MODE_PLACE_WAYPOINT)
            {
                Vector3 hitPoint = getTerrainIntersection(x, y);
                if (hitPoint != Vector3::ZERO)
                {
                    syncSelectedToWalking();
                    addWaypoint(hitPoint);
                }
                return true;
            }

            if (m_CurrentMode == MODE_CLICK_WALK)
            {
                Vector3 hitPoint = getTerrainIntersection(x, y);
                if (hitPoint != Vector3::ZERO)
                {
                    syncSelectedToWalking(); 
                    if (m_WalkingNode) walkTo(hitPoint);
                }
                return true;
            }

            m_bLMouseDown = true;
            m_MouseStart.x = x;
            m_MouseStart.y = y;
            m_MouseStop = m_MouseStart;

            Ogre::Ray mouseRay = m_pCam->getCameraToViewportRay(m_MouseStart.x, m_MouseStart.y);
            mRaySceneQuery->setRay(mouseRay);
            mRaySceneQuery->setSortByDistance(true);
            mRaySceneQuery->setQueryMask(QUERY_MASK_USER_MODEL);
            Ogre::RaySceneQueryResult& result = mRaySceneQuery->execute();

            Ogre::MovableObject* hitObj = nullptr;
            for (auto& itr : result) {
                if (itr.movable &&
                    itr.movable->getName() != "SelectionRect" &&
                    itr.movable->getName().substr(0, 5) != "tile[")
                {
                    hitObj = itr.movable;
                    break;
                }
            }

            if (hitObj) {
                Ogre::SceneNode* hitNode = hitObj->getParentSceneNode();
                if (!hitNode) {
                    m_bDragging = false;
                    m_bLMouseDown = true;
                    m_MouseStart = Ogre::Vector2(m_MouseStart.x, m_MouseStart.y);
                    m_MouseStop = m_MouseStart;
                    mRect->setVisible(true);
                    mRect->setCorners(m_MouseStart.x, m_MouseStart.y, m_MouseStop.x, m_MouseStop.y);
                }
                else if (m_SelectedNodes.find(hitNode) == m_SelectedNodes.end() &&
                    !_key->isKeyDown(OIS::KC_LCONTROL)) {
                    clearSelection();
                    selectObject(hitNode);
                }
                else if (m_SelectedNodes.find(hitNode) == m_SelectedNodes.end()) {
                    selectObject(hitNode);
                }

                syncSelectedToWalking();

                m_bDragging = true;
                m_bLMouseDown = false;

                Vector3 groundPoint = getTerrainIntersection(x, y);
                m_DragStartGroundPoint = groundPoint;

                m_DragOffsets.clear();
                for (auto node : m_SelectedNodes) {
                    m_DragOffsets[node] = node->getPosition() - m_DragStartGroundPoint;
                }
            }
            else {
                m_bDragging = false;
                m_bLMouseDown = true;
                m_MouseStart = Ogre::Vector2(m_MouseStart.x, m_MouseStart.y);
                m_MouseStop = m_MouseStart;
                mRect->setVisible(true);
                mRect->setCorners(m_MouseStart.x, m_MouseStart.y, m_MouseStop.x, m_MouseStop.y);
            }
        }

        if (id == OIS::MB_Right) {
            RMouseButton = true;
        }

        return true;
    }

    virtual bool mouseReleased(const OIS::MouseEvent& arg, OIS::MouseButtonID id)
    {
        if (mTrayMgr->injectMouseUp(arg, id)) return true;

        if (id == OIS::MB_Left)
        {
            m_bDragging = false;
            m_DragOffsets.clear();

            if (m_bLMouseDown) {
                m_bLMouseDown = false;
                mRect->setVisible(false);
                performSelection(m_MouseStart, m_MouseStop);
            }
        }

        if (id == OIS::MB_Right)
        {
            RMouseButton = false;
        }

        return true;
    }

    virtual bool keyPressed(const OIS::KeyEvent& arg)
    {
        if (arg.key == OIS::KC_ESCAPE)
        {
            m_bContinue = false;
        }

        if (arg.key == OIS::KC_R)
        {
            if (_PolyMode == PM_SOLID)
            {
                _PolyMode = Ogre::PolygonMode::PM_WIREFRAME;
            }
            else if (_PolyMode == PM_WIREFRAME)
            {
                _PolyMode = Ogre::PolygonMode::PM_POINTS;
            }
            else if (_PolyMode == PM_POINTS)
            {
                _PolyMode = Ogre::PolygonMode::PM_SOLID;
            }
            m_pCam->setPolygonMode(_PolyMode);
        }

        if (arg.key == OIS::KC_1)
        {
            m_CurrentMode = MODE_SELECT;
            updateModeText();
        }
        else if (arg.key == OIS::KC_2)
        {
            m_CurrentMode = MODE_PLACE_WAYPOINT;
            updateModeText();
        }
        else if (arg.key == OIS::KC_3)
        {
            m_CurrentMode = MODE_CLICK_WALK;
            updateModeText();
        }

        return true;
    }

    virtual bool keyReleased(const OIS::KeyEvent& arg)
    {
        return true;
    }

    bool frameStarted(const Ogre::FrameEvent& evt)
    {
        _key->capture();
        _mouse->capture();

        Ogre::Vector3 TranslateVec(0, 0, 0);

        if (_key->isKeyDown(OIS::KC_W) || _key->isKeyDown(OIS::KC_UP))
        {
            TranslateVec += Ogre::Vector3(0, 0, -10) * m_moveSpeed;
        }
        if (_key->isKeyDown(OIS::KC_S) || _key->isKeyDown(OIS::KC_DOWN))
        {
            TranslateVec += Ogre::Vector3(0, 0, 10) * m_moveSpeed;
        }
        if (_key->isKeyDown(OIS::KC_A) || _key->isKeyDown(OIS::KC_LEFT))
        {
            TranslateVec += Ogre::Vector3(-10, 0, 0) * m_moveSpeed;
        }
        if (_key->isKeyDown(OIS::KC_D) || _key->isKeyDown(OIS::KC_RIGHT))
        {
            TranslateVec += Ogre::Vector3(10, 0, 0) * m_moveSpeed;
        }

        m_pCam->moveRelative(TranslateVec * evt.timeSinceLastFrame);
        mTrayMgr->refreshCursor();
        mTrayMgr->frameRenderingQueued(evt);

        if (m_pCurrentAnimState && m_pCurrentAnimState->getEnabled())
        {
            m_pCurrentAnimState->addTime(evt.timeSinceLastFrame * m_AnimSpeed);
        }

        if (m_WalkAnimState && m_WalkAnimState->getEnabled())
        {
            m_WalkAnimState->addTime(evt.timeSinceLastFrame);
        }

        updatePathWalking(evt.timeSinceLastFrame);

        return m_bContinue;
    }

    void addWaypoint(const Vector3& pos)
    {
        Vector3 wp(pos.x, getTerrainHeight(pos.x, pos.z), pos.z);
        m_WalkList.push_back(wp);

        auto* marker = new WaypointMarker(m_pScenemanage, wp);
        m_WaypointMarkers.push_back(marker);

        if (m_WalkingNode && m_WalkingEntity && m_Direction == Vector3::ZERO && !m_WalkList.empty())
        {
            nextWalkLocation();
        }
    }

    void syncSelectedToWalking()
    {
        
        if (m_SelectedNodes.size() == 1)
        {
            Ogre::SceneNode* selectedNode = *m_SelectedNodes.begin();
            if (selectedNode->numAttachedObjects() > 0)
            {
                Ogre::Entity* ent = dynamic_cast<Ogre::Entity*>(selectedNode->getAttachedObject(0));
                
                if (ent && m_WalkingNode != selectedNode)
                {
                    if (m_WalkAnimState) m_WalkAnimState->setEnabled(false);
                   
                    m_Direction = Vector3::ZERO;
                    setWalkingNode(selectedNode, ent);

                    if (!m_WalkList.empty())
                    {
                        nextWalkLocation();
                    }
                }
            }
        }
        else
        {
            
            if (m_WalkingNode)
            {
                if (m_WalkAnimState) m_WalkAnimState->setEnabled(false);
                setWalkingNode(nullptr, nullptr);
                
                m_Direction = Vector3::ZERO;
                m_Distance = 0.0f;
            }
        }
    }

    void walkTo(const Vector3& target)
    {
        if (!m_WalkingNode)
        {
            if (m_pCurrentNode && m_SelectedNodes.size() == 1)
            {
                Ogre::MovableObject* obj = m_pCurrentNode->getAttachedObject(0);
                Ogre::Entity* ent = dynamic_cast<Ogre::Entity*>(obj);
                if (ent)
                {
                    setWalkingNode(m_pCurrentNode, ent);
                }
            }
            else
            {
                return;
            }
        }

        Vector3 dest(target.x, getTerrainHeight(target.x, target.z), target.z);
        m_WalkList.clear();
        m_WalkList.push_back(dest);
        m_Direction = Vector3::ZERO;

        for (auto* marker : m_WaypointMarkers)
            delete marker;
        m_WaypointMarkers.clear();

        nextWalkLocation();
    }

    bool nextWalkLocation()
    {
        if (m_WalkList.empty())
            return false;
        if (!m_WalkingNode)
            return false;

        m_Destination = m_WalkList.front();
        m_WalkList.pop_front();
        m_Destination.y = getTerrainHeight(m_Destination.x, m_Destination.z);

        m_Direction = m_Destination - m_WalkingNode->getPosition();
        m_Direction.y = 0.0f;
        m_Distance = m_Direction.normalise();

        if (m_Distance < 0.01f)
        {
            m_Direction = Vector3::ZERO;
            m_Distance = 0.0f;
            return false;
        }

        if (m_WalkingEntity)
        {
            Vector3 src = m_WalkingNode->getOrientation() * Vector3::UNIT_X;
            src.normalise();

            if ((1.0f + src.dotProduct(m_Direction)) < 0.0001f)
            {
                m_WalkingNode->yaw(Degree(180));
            }
            else
            {
                Quaternion quat = src.getRotationTo(m_Direction);
                m_WalkingNode->rotate(quat);
            }

            if (m_WalkAnimState)
            {
                m_WalkAnimState->setEnabled(false);
                m_WalkAnimState->setWeight(0.0f);
            }
            if (m_pCurrentAnimState)
            {
                m_pCurrentAnimState->setEnabled(false);
                m_pCurrentAnimState->setWeight(0.0f);
            }
            m_WalkAnimState = safeGetAnimationState(m_WalkingEntity, "Walk");
            if (m_WalkAnimState)
            {
                m_WalkAnimState->setLoop(true);
                m_WalkAnimState->setEnabled(true);
                m_WalkAnimState->setWeight(1.0f);
            }
        }

        return true;
    }

    void updatePathWalking(Real timeSinceLastFrame)
    {
        if (!m_WalkingNode || !m_WalkingEntity)
            return;

        if (m_Direction != Vector3::ZERO)
        {
            Real move = m_WalkSpeed * timeSinceLastFrame;
            m_Distance -= move;

            if (m_Distance <= 0.0f)
            {
                m_WalkingNode->setPosition(m_Destination);
                m_Direction = Vector3::ZERO;
                m_Distance = 0.0f;

                if (m_WalkList.empty())
                {
                    if (m_WalkAnimState)
                    {
                        m_WalkAnimState->setEnabled(false);
                        m_WalkAnimState->setWeight(0.0f);
                    }
                    if (m_WalkingEntity)
                    {
                        m_WalkAnimState = safeGetAnimationState(m_WalkingEntity, "Idle");
                        if (m_WalkAnimState)
                        {
                            m_WalkAnimState->setLoop(true);
                            m_WalkAnimState->setEnabled(true);
                            m_WalkAnimState->setWeight(1.0f);
                        }
                    }
                }
                else
                {
                    nextWalkLocation();
                }
            }
            else
            {
                Vector3 newPos = m_WalkingNode->getPosition() + m_Direction * move;
                newPos.y = getTerrainHeight(newPos.x, newPos.z);
                m_WalkingNode->setPosition(newPos);
            }
        }
        else if (!m_WalkList.empty())
        {
            nextWalkLocation();
        }
    }

    void setWalkingNode(SceneNode* node, Entity* ent)
    {
        m_WalkingNode = node;
        m_WalkingEntity = ent;
    }

    wchar_t* CharToWchar(const char* c)
    {
        int len = MultiByteToWideChar(CP_ACP, 0, c, -1, NULL, 0);
        wchar_t* m_wchar = new wchar_t[len];
        MultiByteToWideChar(CP_ACP, 0, c, -1, m_wchar, len);
        return m_wchar;
    }

    String WcharToChar(const wchar_t* wp, size_t m_encode = CP_ACP)
    {
        if (!wp || wp[0] == L'\0') return "";
        String str;
        int len = WideCharToMultiByte(m_encode, 0, wp, wcslen(wp), NULL, 0, NULL, NULL);
        if (len <= 0) return "";
        char* m_char = new char[len + 1];
        WideCharToMultiByte(m_encode, 0, wp, wcslen(wp), m_char, len, NULL, NULL);
        m_char[len] = '\0';
        str = m_char;
        delete[] m_char;
        return str;
    }

    void setupAnimation(Ogre::Entity* ent, int index)
    {
        if (!ent || !ent->getAllAnimationStates()) return;

        Ogre::AnimationStateSet* animSet = ent->getAllAnimationStates();

        Ogre::AnimationStateIterator it = animSet->getAnimationStateIterator();
        while (it.hasMoreElements())
        {
            Ogre::AnimationState* anim = it.getNext();
            anim->setEnabled(false);
            anim->setWeight(0);
        }

        it = animSet->getAnimationStateIterator();
        int count = 0;
        while (it.hasMoreElements())
        {
            Ogre::AnimationState* anim = it.getNext();
            if (count == index)
            {
                m_pCurrentAnimState = anim;
                m_pCurrentAnimState->setEnabled(true);
                m_pCurrentAnimState->setLoop(true);
                m_pCurrentAnimState->setWeight(1.0f);
                m_pCurrentAnimState->setTimePosition(0);

                if (m_pAnimNameBox)
                {
                    Ogre::String name = anim->getAnimationName();
                    m_pAnimNameBox->setText("Anim:\n" + name);
                }

                break;
            }
            count++;
        }
    }

    void clearSelection()
    {
        for (auto node : m_SelectedNodes) {
            setNodeBoundingBox(node, false);
        }
        m_SelectedNodes.clear();
        m_pCurrentNode = nullptr;
        m_pCurrentAnimState = nullptr;
        m_CurrentAnimIndex = 0;

        
        if (m_pInfoBox) m_pInfoBox->setText("No object selected.");
        if (m_pAnimNameBox) m_pAnimNameBox->setText("No animation.");
    }

    void selectObject(Ogre::SceneNode* node)
    {
        if (!node) return;

        m_SelectedNodes.insert(node);
        m_pCurrentNode = node;
        setNodeBoundingBox(node, true);
        InitUIValue();


        if (m_pInfoBox) {
            if (m_SelectedNodes.size() == 1) {
                // 只有 1 个模型时，显示详细名字和坐标 (取整让显示更清爽)
                Ogre::Vector3 pos = node->getPosition();
                Ogre::String info = "Name: " + node->getName() + "\n";
                info += "Pos: " + Ogre::StringConverter::toString((int)pos.x) + ", "
                    + Ogre::StringConverter::toString((int)pos.y) + ", "
                    + Ogre::StringConverter::toString((int)pos.z);
                m_pInfoBox->setText(info);
            }
            else if (m_SelectedNodes.size() > 1) {
                // 多选时，显示 Many Selected
                m_pInfoBox->setText("Multiple Selected\nTotal: " + Ogre::StringConverter::toString(m_SelectedNodes.size()));
            }
        }

        if (m_SelectedNodes.size() > 1)
        {
            m_pCurrentAnimState = nullptr;
            m_CurrentAnimIndex = 0;
            if (m_pAnimNameBox)
                m_pAnimNameBox->setText("Multiple Selected");
            return;
        }

        if (node->numAttachedObjects() > 0)
        {
            Ogre::MovableObject* obj = node->getAttachedObject(0);
            if (!obj) return;
            Ogre::Entity* ent = dynamic_cast<Ogre::Entity*>(obj);

            if (!ent) return;

            if (ent->getAllAnimationStates())
            {
                Ogre::AnimationStateSet* animSet = ent->getAllAnimationStates();

                if (!animSet->getAnimationStateIterator().hasMoreElements())
                {
                    if (m_pAnimNameBox)
                        m_pAnimNameBox->setText("No animation.");
                }
                else
                {
                    m_CurrentAnimIndex = 0;
                    setupAnimation(ent, m_CurrentAnimIndex);
                }
            }
            else
            {
                if (m_pAnimNameBox)
                    m_pAnimNameBox->setText("No animation.");
            }
        }
    }

    void performSelection(const Ogre::Vector2& first, const Ogre::Vector2& second)
    {
        if (!_key->isKeyDown(OIS::KC_LCONTROL) && !_key->isKeyDown(OIS::KC_RCONTROL)) {
            clearSelection();
        }

        if ((first - second).squaredLength() < 0.0001f) {
            Ogre::Ray mouseRay = m_pCam->getCameraToViewportRay(first.x, first.y);
            mRaySceneQuery->setRay(mouseRay);
            mRaySceneQuery->setSortByDistance(true);
            mRaySceneQuery->setQueryMask(QUERY_MASK_USER_MODEL);
            Ogre::RaySceneQueryResult& result = mRaySceneQuery->execute();
            for (auto itr = result.begin(); itr != result.end(); ++itr) {
                if (itr->movable && itr->movable->getName() != "SelectionRect"&& itr->movable->getName().substr(0, 5) != "tile[") {
                    Ogre::SceneNode* hitNode = itr->movable->getParentSceneNode();
                    if (hitNode)
                        selectObject(hitNode);
                    break;
                }
            }
        }
        else
        {
            float left = first.x, right = second.x;
            float top = first.y, bottom = second.y;
            if (left > right) std::swap(left, right);
            if (top > bottom) std::swap(top, bottom);

            Ogre::Ray topLeft = m_pCam->getCameraToViewportRay(left, top);
            Ogre::Ray topRight = m_pCam->getCameraToViewportRay(right, top);
            Ogre::Ray bottomLeft = m_pCam->getCameraToViewportRay(left, bottom);
            Ogre::Ray bottomRight = m_pCam->getCameraToViewportRay(right, bottom);

            Ogre::PlaneBoundedVolume vol;
            vol.planes.push_back(Ogre::Plane(topLeft.getPoint(3), topRight.getPoint(3), bottomRight.getPoint(3)));
            vol.planes.push_back(Ogre::Plane(topLeft.getOrigin(), topLeft.getPoint(100), topRight.getPoint(100)));
            vol.planes.push_back(Ogre::Plane(topLeft.getOrigin(), bottomLeft.getPoint(100), topLeft.getPoint(100)));
            vol.planes.push_back(Ogre::Plane(bottomLeft.getOrigin(), bottomRight.getPoint(100), bottomLeft.getPoint(100)));
            vol.planes.push_back(Ogre::Plane(topRight.getOrigin(), topRight.getPoint(100), bottomRight.getPoint(100)));

            Ogre::PlaneBoundedVolumeList volList;
            volList.push_back(vol);
            mVolQuery->setVolumes(volList);
            mVolQuery->setQueryMask(QUERY_MASK_USER_MODEL);

            Ogre::SceneQueryResult result = mVolQuery->execute();
            for (auto itr = result.movables.begin(); itr != result.movables.end(); ++itr) {
                if ((*itr)->getName() != "SelectionRect" &&
                    (*itr)->getName().substr(0, 5) != "tile[")
                {
                    Ogre::SceneNode* hitNode = (*itr)->getParentSceneNode();
                    if (hitNode)
                        selectObject(hitNode);
                }
            }
        }
        syncSelectedToWalking();
    }

    void makeEntityVector()
    {
        static int num = 0;
        Ogre::DisplayString EntityArray[] = {
          "robot",  "Sinbad", "penguin"
        };

        for (int i = 0; i < size(EntityArray); i++)
        {
            m_LoadedEntity.push_back(EntityArray[i]);
        }
    }

    void getAllEntity(Ogre::SceneNode* rootNode, std::vector<Ogre::Entity*>& entityVec)
    {
        if (!rootNode)
            return;
        else
        {
            unsigned short numChildren = rootNode->numChildren();
            for (unsigned short nodeIndex = 0; nodeIndex < numChildren; ++nodeIndex)
            {
                Ogre::SceneNode* childNode = (Ogre::SceneNode*)rootNode->getChild(nodeIndex);
                unsigned short numEntity = childNode->numAttachedObjects();
                for (unsigned short entityIndex = 0; entityIndex < numEntity; ++entityIndex)
                {
                    Ogre::Entity* ent = (Ogre::Entity*)childNode->getAttachedObject(entityIndex);
                    entityVec.push_back(ent);
                }
                getAllEntity(childNode, entityVec);
            }
        }
    }

    void setNodeBoundingBox(Ogre::SceneNode* node, bool show)
    {
        if (!node) return;
        node->showBoundingBox(show);
    }

    void DestroySceneNode(Ogre::SceneNode* pSceneNode)
    {
        if (!pSceneNode)
            return;

        // 1. 先将绑定的物体收集到临时数组，防止边遍历边解绑导致迭代器失效
        std::vector<Ogre::MovableObject*> objectsToDestroy;
        Ogre::SceneNode::ObjectIterator iterMovableObject = pSceneNode->getAttachedObjectIterator();
        while (iterMovableObject.hasMoreElements())
        {
            Ogre::MovableObject* obj = iterMovableObject.getNext();
            if (obj)
                objectsToDestroy.push_back(obj);
        }

        for (size_t i = 0; i < objectsToDestroy.size(); ++i)
        {
            pSceneNode->detachObject(objectsToDestroy[i]);
            m_pScenemanage->destroyMovableObject(objectsToDestroy[i]);
        }

        // 2. 先收集子节点，再递归销毁，防止销毁子节点时修改父节点结构导致子节点迭代器失效
        std::vector<Ogre::SceneNode*> childrenToDestroy;
        Ogre::SceneNode::ChildNodeIterator iterChild = pSceneNode->getChildIterator();
        while (iterChild.hasMoreElements())
        {
            Ogre::SceneNode* child = static_cast<Ogre::SceneNode*>(iterChild.getNext());
            if (child)
                childrenToDestroy.push_back(child);
        }

        for (size_t i = 0; i < childrenToDestroy.size(); ++i)
        {
            DestroySceneNode(childrenToDestroy[i]);
        }

        // 3. 安全销毁节点自身，并重置业务层悬空指针
        if (pSceneNode->getName() != "ModelsNode")
        {
            if (pSceneNode == m_pCurrentNode)
                m_pCurrentNode = nullptr;

            m_pScenemanage->destroySceneNode(pSceneNode);
        }
    }

    void createNodeEntity(Ogre::String& name)
    {
        clearSelection();

        Ogre::String EntityName = name + "_" + Ogre::StringConverter::toString(ModelNums);
        Ogre::String modelFileName = name + ".mesh";

        Ogre::Ray camRay = m_pCam->getCameraToViewportRay(0.5, 0.5);
        Vector3 spawnPos = getTerrainIntersection(0.5f, 0.5f);

        if (spawnPos == Vector3::ZERO)
        {
            Ogre::Vector3 camPos = m_pCam->getDerivedPosition();
            Ogre::Vector3 camDir = m_pCam->getDerivedDirection();
            spawnPos = camPos + (camDir * 300);
            spawnPos.y = getTerrainHeight(spawnPos.x, spawnPos.z);
        }

        m_pCurrentNode = m_pModelsNode->createChildSceneNode(EntityName);
        m_pCurrentNode->setPosition(spawnPos);
        m_pCurrentNode->setScale(0.1f, 0.1f, 0.1f);
        m_pCurrentNode->setOrientation(Ogre::Quaternion::IDENTITY);

        Ogre::Entity* ent = m_pScenemanage->createEntity(EntityName, modelFileName);
        ent->setQueryFlags(QUERY_MASK_USER_MODEL);
        m_pCurrentNode->attachObject(ent);

        ModelNums++;
        selectObject(m_pCurrentNode);

        if (!m_WalkingNode)
        {
            setWalkingNode(m_pCurrentNode, ent);
        }

        InitUIValue();
    }

    void updateModeText()
    {
        String modeStr;
        switch (m_CurrentMode)
        {
        case MODE_SELECT: modeStr = "Select"; break;
        case MODE_PLACE_WAYPOINT: modeStr = "Place Waypoint"; break;
        case MODE_CLICK_WALK: modeStr = "Click to Walk"; break;
        }
        String fullText = "Mode [1/2/3]: " + modeStr;
        if (m_pModeLabel)
            m_pModeLabel->setCaption(fullText);
    }

    void setupUI()
    {
        Ogre::FontManager::getSingleton().getByName("SdkTrays/Caption")->load();
        Ogre::FontManager::getSingleton().getByName("SdkTrays/Value")->load();
        mTrayMgr->createButton(TL_TOPLEFT, "Quit", Ogre::DisplayString(L"退出"), 150);
        mTrayMgr->createButton(TL_TOPLEFT, "NewScene", Ogre::DisplayString(L"新建场景"), 150);
        mTrayMgr->createButton(TL_TOPLEFT, "Save", Ogre::DisplayString(L"保存场景"), 150);
        mTrayMgr->createButton(TL_TOPLEFT, "Open", Ogre::DisplayString(L"打开场景"), 150);

        m_pMyModelMenu = mTrayMgr->createThickSelectMenu(TL_TOPRIGHT, "ModelsMenu", Ogre::DisplayString(L"模型"), 200, 20);

        m_ScaleX = mTrayMgr->createThickSlider(TL_TOPRIGHT, "MySliderScale", "ScaleXYZ", 200, 80, 0, 3, 60);

        m_pEffectLabel = mTrayMgr->createLabel(TL_TOPRIGHT, "EffectLabel", "FX: None", 200);

        makeEntityVector();
        m_pMyModelMenu->setItems(m_LoadedEntity);

        mTrayMgr->createButton(TL_BOTTOMLEFT, "CreateEntity", Ogre::DisplayString(L"创建模型"), 150);
        mTrayMgr->createButton(TL_BOTTOMLEFT, "DeleteEntity", Ogre::DisplayString(L"删除选中"), 150);

        mTrayMgr->createButton(TL_BOTTOMLEFT, "Play", Ogre::DisplayString(L"播放"), 80);
        mTrayMgr->createButton(TL_BOTTOMLEFT, "Pause", Ogre::DisplayString(L"暂停"), 80);
        mTrayMgr->createButton(TL_BOTTOMLEFT, "NextAnim", Ogre::DisplayString(L"下一个动画"), 80);
        mTrayMgr->createButton(TL_BOTTOMLEFT, "Faster", Ogre::DisplayString(L"加速"), 80);
        mTrayMgr->createButton(TL_BOTTOMLEFT, "Slower", Ogre::DisplayString(L"减速"), 80);

        mTrayMgr->createButton(TL_BOTTOMLEFT, "ToggleCompositor", Ogre::DisplayString(L"切换特效"), 120);

        m_pModeLabel = mTrayMgr->createLabel(TL_TOP, "ModeLabel", "Mode [1/2/3]: Select", 700);
        updateModeText();

        m_pInfoBox = mTrayMgr->createTextBox(TL_BOTTOMRIGHT, "InfoTextBox", Ogre::DisplayString(L"对象信息"), 250, 150);
        m_pInfoBox->setText("No object selected.");

        m_pAnimNameBox = mTrayMgr->createTextBox(
            TL_BOTTOMRIGHT,
            "AnimNameBox",
            Ogre::DisplayString(L"动画"),
            250,
            100
        );
        m_pAnimNameBox->setText("No animation.");
    }

    void InitUIValue()
    {
        if (m_pCurrentNode == NULL)
        {
            return;
        }

        m_ScaleX->setValue(m_pCurrentNode->getScale().x, false);
    }

    virtual void buttonHit(Button* button)
    {
        if (button->getName() == "Quit")
        {
            m_bContinue = false;
        }
        if (button->getName() == "CreateEntity")
        {
            if (m_CurrentName != "")
            {
                createNodeEntity(m_CurrentName);
            }
            InitUIValue();
        }
        if (button->getName() == "Save")
        {
            saveFile();
        }

        if (button->getName() == "NewScene")
        {
            m_WalkingNode = nullptr;
            m_WalkingEntity = nullptr;
            m_WalkAnimState = nullptr;
            m_Direction = Vector3::ZERO;
            m_Distance = 0.0f;
            m_WalkList.clear();
            for (auto* marker : m_WaypointMarkers) delete marker;
            m_WaypointMarkers.clear();
            clearSelection();

          
            DestroySceneNode(m_pModelsNode);

            
            ModelNums = 0;
            InitUIValue();
            if (m_pInfoBox) m_pInfoBox->setText("New Scene Created.");
        }
        if (button->getName() == "Open")
        {
            m_WalkingNode = nullptr;
            m_WalkingEntity = nullptr;
            m_WalkAnimState = nullptr;
            m_Direction = Vector3::ZERO;
            m_Distance = 0.0f;
            m_WalkList.clear();
            for (auto* marker : m_WaypointMarkers) delete marker;
            m_WaypointMarkers.clear();
            clearSelection();

            DestroySceneNode(m_pModelsNode);
            openScene();
            InitUIValue();
        }
        if (button->getName() == "DeleteEntity")
        {
            m_pCurrentAnimState = nullptr;
            m_CurrentAnimIndex = 0;

            for (auto node : m_SelectedNodes) {
                if (node != nullptr) {
                    if (node == m_WalkingNode)
                    {
                        m_WalkingNode = nullptr;
                        m_WalkingEntity = nullptr;
                        m_WalkAnimState = nullptr;
                        m_Direction = Vector3::ZERO;
                        m_Distance = 0.0f;
                        m_WalkList.clear();
                        for (auto* marker : m_WaypointMarkers)
                            delete marker;
                        m_WaypointMarkers.clear();
                    }
                    DestroySceneNode(node);
                }
            }
            m_SelectedNodes.clear();
            m_pCurrentNode = nullptr;
            InitUIValue();
            if (m_pInfoBox) m_pInfoBox->setText("Deleted.");
        }
        if (button->getName() == "Play") {
            m_AnimSpeed = 1.0f;
        }
        if (button->getName() == "NextAnim")
        {
            if (m_SelectedNodes.size() > 1)
            {
                if (m_pAnimNameBox)
                    m_pAnimNameBox->setText("Multiple Selected");
                return;
            }
            if (m_pCurrentNode && m_pCurrentNode->numAttachedObjects() > 0)
            {
                Ogre::MovableObject* obj = m_pCurrentNode->getAttachedObject(0);
                if (!obj) return;
                Ogre::Entity* ent = dynamic_cast<Ogre::Entity*>(obj);
                if (ent && ent->getAllAnimationStates())
                {
                    int totalAnims = 0;
                    Ogre::AnimationStateIterator itCount = ent->getAllAnimationStates()->getAnimationStateIterator();
                    while (itCount.hasMoreElements()) { itCount.getNext(); totalAnims++; }

                    if (totalAnims > 0)
                    {
                        m_CurrentAnimIndex = (m_CurrentAnimIndex + 1) % totalAnims;
                        setupAnimation(ent, m_CurrentAnimIndex);
                    }
                }
            }
        }
        if (button->getName() == "Pause") m_AnimSpeed = 0.0f;
        if (button->getName() == "Faster") m_AnimSpeed *= 1.5f;
        if (button->getName() == "Slower") m_AnimSpeed *= 0.5f;

        if (button->getName() == "ToggleCompositor")
        {
            if (m_pViewport)
            {
                m_CompositorEnabled = !m_CompositorEnabled;
                try {
                    Ogre::CompositorManager::getSingleton().setCompositorEnabled(m_pViewport, "CyberPunk", m_CompositorEnabled);

                    
                    if (m_pEffectLabel)
                    {
                        if (m_CompositorEnabled)
                            m_pEffectLabel->setCaption("FX: CyberPunk");
                        else
                            m_pEffectLabel->setCaption("FX: None");
                    }
                }
                catch (Ogre::Exception& e) {
                    Ogre::LogManager::getSingleton().logMessage("ToggleCompositor Error: " + e.getFullDescription());
                }
                catch (...) {
                    Ogre::LogManager::getSingleton().logMessage("ToggleCompositor Unknown Error");
                }
            }
        }
    }

    void saveFile()
    {
        Ogre::SceneNode* modelsNode = m_pScenemanage->getSceneNode("ModelsNode");
        if (!modelsNode) return;

        std::vector<Ogre::Entity*> EntityVec;
        getAllEntity(modelsNode, EntityVec);
        std::vector<EntityAttribue*> entityAttriVec;
        for (int i = 0; i < EntityVec.size(); i++)
        {
            EntityAttribue* EA = new EntityAttribue();
            Ogre::SceneNode* pParentNode = EntityVec[i]->getParentSceneNode();
            if (!pParentNode) { delete EA; continue; }
            EA->SceneNode = pParentNode->getName();
            String entityName = EntityVec[i]->getName();
            EA->EntityName = EntityVec[i]->getName();

            int nl = entityName.find('_');
            String MeshName;
            if (nl != -1) entityName = entityName.substr(0, nl);
            MeshName = entityName + ".mesh";
            EA->mesh = MeshName;
            EA->PosX = pParentNode->getPosition().x;
            EA->PosY = pParentNode->getPosition().y;
            EA->PosZ = pParentNode->getPosition().z;

            EA->ScaleX = pParentNode->getScale().x;
            EA->ScaleY = pParentNode->getScale().y;
            EA->ScaleZ = pParentNode->getScale().z;

            Quaternion qua = pParentNode->getOrientation();
            Vector3 Axis;
            Degree DegreeValue;
            qua.ToAngleAxis(DegreeValue, Axis);
            Axis = Axis * DegreeValue.valueDegrees();

            EA->RotationX = Axis.x;
            EA->RotationY = Axis.y;
            EA->RotationZ = Axis.z;
            entityAttriVec.push_back(EA);
        }
        write_json("MyScene", entityAttriVec);
        for (size_t i = 0; i < entityAttriVec.size(); i++) {
            delete entityAttriVec[i];
        }
        entityAttriVec.clear();
    }

    void openScene()
    {
        ModelNums = 0;
        std::vector<EntityAttribue*> entityAttri;
        read_json("MyScene", entityAttri);
        Ogre::SceneNode* pModelsNode = m_pScenemanage->getSceneNode("ModelsNode");

        if (!pModelsNode)
        {
            pModelsNode = m_pScenemanage->getRootSceneNode()->createChildSceneNode("ModelsNode");
        }

        for (int i = 0; i < entityAttri.size(); i++)
        {
            int nl = entityAttri[i]->mesh.find('.');
            String EntityName;
            if (nl != -1) EntityName = entityAttri[i]->mesh.substr(0, nl);
            else EntityName = entityAttri[i]->mesh;
            cout << EntityName << endl;

            EntityName = EntityName + "_" + Ogre::StringConverter::toString(ModelNums);

            Ogre::SceneNode* pNode = pModelsNode->createChildSceneNode(EntityName);
            Ogre::Entity* ent = m_pScenemanage->createEntity(EntityName, entityAttri[i]->mesh);
            ent->setQueryFlags(QUERY_MASK_USER_MODEL);
            pNode->attachObject(ent);

            Real posY = getTerrainHeight(entityAttri[i]->PosX, entityAttri[i]->PosZ);
            pNode->setPosition(Vector3(entityAttri[i]->PosX, posY, entityAttri[i]->PosZ));
            pNode->setScale(Vector3(entityAttri[i]->ScaleX, entityAttri[i]->ScaleY, entityAttri[i]->ScaleZ));

            Ogre::Quaternion qX(Ogre::Degree(entityAttri[i]->RotationX), Ogre::Vector3::UNIT_X);
            Ogre::Quaternion qY(Ogre::Degree(entityAttri[i]->RotationY), Ogre::Vector3::UNIT_Y);
            Ogre::Quaternion qZ(Ogre::Degree(entityAttri[i]->RotationZ), Ogre::Vector3::UNIT_Z);
            pNode->setOrientation(qY * qX * qZ);

            m_pCurrentNode = pNode;
            ModelNums++;

            if (!m_WalkingNode)
            {
                setWalkingNode(pNode, ent);
            }

        }
            for (size_t i = 0; i < entityAttri.size(); i++) {
                delete entityAttri[i];
            }
            entityAttri.clear();
        
    }

    virtual void sliderMoved(Slider* slider)
    {
        if (!m_pCurrentNode) return;

        if (slider->getName() == "MySliderScale")
        {
            Ogre::Vector3 Vect(slider->getValue(), slider->getValue(), slider->getValue());
            m_pCurrentNode->setScale(Vect);
        }
    }

    virtual void itemSelected(SelectMenu* menu)
    {
        if (menu->getName() == "ModelsMenu")
        {
            m_CurrentName = WcharToChar(menu->getSelectedItem().asWStr_c_str());
            std::cout << "Selected model: " << m_CurrentName << std::endl;
        }
    }

private:
    OIS::InputManager* _man;
    OIS::Keyboard* _key;
    OIS::Mouse* _mouse;
    float m_moveSpeed;
    Ogre::Camera* m_pCam;
    Ogre::Timer _timer;
    Ogre::PolygonMode _PolyMode;

    SdkTrayManager* mTrayMgr;
    SelectMenu* m_pMyMenu;
    SelectMenu* m_pMyModelMenu;
    bool RMouseButton;
    bool m_bContinue;
    OgreBites::DisplayVector m_LoadedEntity;
    Ogre::SceneManager* m_pScenemanage;
    Ogre::SceneNode* m_pCurrentNode;
    Ogre::SceneNode* m_pModelsNode;
    Ogre::String m_CurrentName;
    int ModelNums;

    Slider* m_ScaleX;
    Ogre::AnimationState* m_pCurrentAnimState;
    int m_CurrentAnimIndex;
    float m_AnimSpeed;

    Ogre::RaySceneQuery* mRaySceneQuery;

    TextBox* m_pInfoBox;
    TextBox* m_pAnimNameBox;
    OgreBites::Label* m_pModeLabel;
    OgreBites::Label* m_pEffectLabel;

    SelectionRectangle* mRect;
    Ogre::PlaneBoundedVolumeListSceneQuery* mVolQuery;
    bool m_bLMouseDown;
    Ogre::Vector2 m_MouseStart;
    Ogre::Vector2 m_MouseStop;
    std::set<Ogre::SceneNode*> m_SelectedNodes;

    bool m_bDragging;
    std::map<Ogre::SceneNode*, Ogre::Vector3> m_DragOffsets;
    Ogre::Vector3 m_DragStartGroundPoint;

    InteractionMode m_CurrentMode;
    std::deque<Vector3> m_WalkList;
    Vector3 m_Direction;
    Vector3 m_Destination;
    Real m_Distance;
    Real m_WalkSpeed;
    SceneNode* m_WalkingNode;
    Entity* m_WalkingEntity;
    AnimationState* m_WalkAnimState;
    std::vector<WaypointMarker*> m_WaypointMarkers;

    bool m_CompositorEnabled;
    Ogre::Viewport* m_pViewport;
};

class Application
{
public:
    Application()
    {
        mRoot = nullptr;
        mWindow = nullptr;
        mSceneMgr = nullptr;
        mCamera = nullptr;
        mViewport = nullptr;
        mFrameListener = nullptr;
    }

    ~Application()
    {
        if (mFrameListener)
        {
            delete mFrameListener;
        }
        if (mRoot)
        {
            delete mRoot;
        }
    }

    void go()
    {
        if (!setup())
            return;

        mRoot->startRendering();

        destroyScene();
    }

protected:

    bool setup()
    {
        mRoot = new Ogre::Root("plugins_d.cfg", "ogre.cfg", "Ogre.log");

        if (!mRoot->restoreConfig())
        {
            if (!mRoot->showConfigDialog())
                return false;
        }

        mWindow = mRoot->initialise(true, "Ogre Application");

        Ogre::ConfigFile cf;
        cf.load("resources_d.cfg");


        Ogre::ConfigFile::SectionIterator seci = cf.getSectionIterator();
        Ogre::String secName, typeName, archName;
        while (seci.hasMoreElements())
        {
            secName = seci.peekNextKey();
            Ogre::ConfigFile::SettingsMultiMap* settings = seci.getNext();
            Ogre::ConfigFile::SettingsMultiMap::iterator i;
            for (i = settings->begin(); i != settings->end(); ++i)
            {
                typeName = i->first;
                archName = i->second;

                
                Ogre::ResourceGroupManager::getSingleton().addResourceLocation(
                    archName, typeName, secName);
            }
        }


        chooseSceneManager();
        createCamera();
        createViewports();

        Ogre::ResourceGroupManager::getSingleton().initialiseAllResourceGroups();

        createScene();
        createFrameListener();

        return true;
    }

    void chooseSceneManager()
    {
        mSceneMgr = mRoot->createSceneManager(ST_EXTERIOR_CLOSE);
    }

    void createCamera()
    {
        mCamera = mSceneMgr->createCamera("MyCamera1");
        mCamera->setPosition(0, 100, 200);
        mCamera->lookAt(0, 0, 0);
        mCamera->setNearClipDistance(5);
    }

    void createViewports()
    {
        mViewport = mWindow->addViewport(mCamera);
        mViewport->setBackgroundColour(ColourValue(0.9f, 0.9f, 0.9f));
        mCamera->setAspectRatio(Real(mViewport->getActualWidth()) / Real(mViewport->getActualHeight()));
    }

    void createScene()
    {
        ColourValue fadeColour(0.9f, 0.9f, 0.9f);
        mViewport->setBackgroundColour(fadeColour);
        mSceneMgr->setFog(FOG_LINEAR, fadeColour, 0.0, 50, 515);

        mSceneMgr->setWorldGeometry("terrain.cfg");

        mSceneMgr->setSkyDome(true, "Examples/CloudySky", 5, 8, 500);

        Ogre::Light* light = mSceneMgr->createLight("Light1");
        light->setType(Ogre::Light::LT_DIRECTIONAL);
        light->setDirection(Ogre::Vector3(1, -1, 0));

        mSceneMgr->setAmbientLight(ColourValue(0.5f, 0.5f, 0.5f));
        mSceneMgr->setShadowTechnique(Ogre::SHADOWTYPE_STENCIL_ADDITIVE);

        Ogre::CompositorManager& compMgr = Ogre::CompositorManager::getSingleton();
        try {
            Ogre::CompositorInstance* instance = compMgr.addCompositor(mViewport, "CyberPunk");
            compMgr.setCompositorEnabled(mViewport, "CyberPunk", false);
        }
        catch (Ogre::Exception& e) {
            Ogre::LogManager::getSingleton().logMessage("Compositor Setup Error: " + e.getFullDescription());
        }
        catch (...) {
            Ogre::LogManager::getSingleton().logMessage("Compositor Setup Unknown Error");
        }
    }

    void createFrameListener()
    {
        mFrameListener = new AppFrameListener(mWindow, mCamera, mSceneMgr);
        mFrameListener->setViewport(mViewport);
        mRoot->addFrameListener(mFrameListener);
    }

    void destroyScene()
    {
    }

private:
    Ogre::Root* mRoot;
    Ogre::RenderWindow* mWindow;
    Ogre::SceneManager* mSceneMgr;
    Ogre::Camera* mCamera;
    Ogre::Viewport* mViewport;
    AppFrameListener* mFrameListener;
};

int main(void)
{
    Application app;
    app.go();
    return 0;
}
