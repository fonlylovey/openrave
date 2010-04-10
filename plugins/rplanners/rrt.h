// Copyright (C) 2006-2009 Rosen Diankov (rdiankov@cs.cmu.edu)
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
#ifndef  BIRRT_PLANNER_H
#define  BIRRT_PLANNER_H

#include "rplanners.h"

const int   NUM_OPT_ITERATIONS = 100;  // optimization iterations

template <typename Node>
class RrtPlanner : public PlannerBase
{
public:
    
 RrtPlanner(EnvironmentBasePtr penv) : PlannerBase(penv)
    {
        __description = "Rosen's RRT planner";
        _report.reset(new COLLISIONREPORT());
        _bInit = false;
    }
    virtual ~RrtPlanner() {}

    virtual RobotBasePtr GetRobot() { return _robot; }

    virtual bool InitPlan(RobotBasePtr pbase, PlannerParametersConstPtr pparams)
    {
        EnvironmentMutex::scoped_lock lock(GetEnv()->GetMutex());
        GetParameters().copy(pparams);
        _robot = pbase;

        RobotBase::RobotStateSaver savestate(_robot);
        if( (int)GetParameters().vinitialconfig.size() != GetParameters().GetDOF() ) {
            RAVELOG_ERRORA(str(boost::format("initial config wrong dim: %d\n")%GetParameters().vinitialconfig.size()));
            return false;
        }

        if(CollisionFunctions::CheckCollision(GetParameters(),_robot,GetParameters().vinitialconfig, _report)) {
            RAVELOG_DEBUGA("BirrtPlanner::InitPlan - Error: Initial configuration in collision\n");
            return false;
        }
            
        _randomConfig.resize(GetParameters().GetDOF());
    
        // set up the initial state
        if( !!GetParameters()._constraintfn ) {
            GetParameters()._setstatefn(GetParameters().vinitialconfig);
            if( !GetParameters()._constraintfn(GetParameters().vinitialconfig, GetParameters().vinitialconfig,0) ) {
                // failed
                RAVELOG_WARNA("initial state rejected by constraint fn\n");
                return false;
            }
        }

        _treeForward.Reset(shared_planner(), GetParameters().GetDOF());;
        _treeForward._fStepLength = GetParameters()._fStepLength;
        _treeForward._distmetricfn = GetParameters()._distmetricfn;
        _treeForward.AddNode(-1, GetParameters().vinitialconfig);

        _bInit = true;
        RAVELOG_DEBUGA("RrtPlanner::InitPlan - RRT Planner Initialized\n");
        return true;
    }

    virtual PlannerParameters& GetParameters() = 0;


    /// optimize the computed path over a number of iterations
    virtual void _OptimizePath(list<Node*>& path, int numiterations)
    {
        if( path.size() <= 2 )
            return;
        RAVELOG_DEBUG("optimizing path - path shortcut\n");

        typename list<Node*>::iterator startNode, endNode;
        vector< vector<dReal> > vconfigs;

        int nrejected = 0;
        int i = numiterations;
        while(i > 0 && nrejected < (int)path.size()+4 ) {

            --i;

            // pick a random node on the path, and a random jump ahead
            int endIndex = 2+(RaveRandomInt()%((int)path.size()-2));
            int startIndex = RaveRandomInt()%(endIndex-1);
        
            startNode = path.begin();
            advance(startNode, startIndex);
            endNode = startNode;
            advance(endNode, endIndex-startIndex);
            nrejected++;

            // check if the nodes can be connected by a straight line
            vconfigs.resize(0);
            if (CollisionFunctions::CheckCollision(GetParameters(),_robot,(*startNode)->q, (*endNode)->q, OPEN, &vconfigs)) {

                if( nrejected++ > (int)path.size()+8 )
                    break;
                continue;
            }

            ++startNode;
            FOREACHC(itc, vconfigs)
                path.insert(startNode, _treeForward._nodes.at(_treeForward.AddNode(-1,*itc)));
            // splice out in-between nodes in path
            path.erase(startNode, endNode);
            nrejected = 0;

            if( path.size() <= 2 )
                return;
        }
    }

    virtual void _OptimizePathSingle(list<Node*>& path, int numiterations)
    {
        if( path.size() <= 2 )
            return;
        RAVELOG_DEBUG("optimizing path - path shortcut single dimension\n");

        typename list<Node*>::iterator startNode, prevNode, nextNode, endNode;
        vector< vector<dReal> > vconfigs;
        vector<dReal> qprev, qnext;
        vector<dReal> vdists;

        int nrejected = 0;
        int i = numiterations;
        while(i > 0 && nrejected < (int)path.size()+4 ) {
            --i;
            
            // pick a random node on the path, and a random jump ahead
            int endIndex = 2+(RaveRandomInt()%((int)path.size()-2));
            int startIndex = RaveRandomInt()%(endIndex-1);
            int dim = RaveRandomInt()%GetParameters().GetDOF();
            
            nrejected++;
            startNode = path.begin(); advance(startNode, startIndex);
            endNode = startNode;
            advance(endNode, endIndex-startIndex);
            
            prevNode = startNode;
            nextNode = prevNode; ++nextNode;
            vdists.resize(0);
            vdists.push_back(0);
            for(int j = 1; j < endIndex-startIndex; ++j,++nextNode) {
                vdists.push_back(vdists.back()+GetParameters()._distmetricfn((*prevNode)->q,(*nextNode)->q));
                prevNode = nextNode;
            }

            if( vdists.back() <= 0 ) {
                nrejected = 0;
                ++startNode;
                path.erase(startNode, endNode);
                continue;
            }

            // normalize distances and start checking collision
            dReal itotaldist = 1.0f/vdists.back();

            // check if the nodes can be connected by a linear interpolation of dim
            vconfigs.resize(0);
            prevNode = startNode;
            nextNode = prevNode; ++nextNode;
            qprev = (*prevNode)->q;
            bool bCanConnect = true;
            for(int j = 1; j < endIndex-startIndex; ++j,++nextNode) {
                qnext = (*nextNode)->q;
                qnext[dim] = vdists[j]*itotaldist*((*endNode)->q[dim]-(*startNode)->q[dim])+(*startNode)->q[dim];
                if (CollisionFunctions::CheckCollision(GetParameters(),_robot,qprev, qnext, OPEN_START, &vconfigs)) {
                    bCanConnect = false;
                    break;
                }
                qprev = qnext;
                prevNode = nextNode;
            }

            if( !bCanConnect ) {
                if( nrejected++ > (int)path.size()+8 )
                    break;
                continue;
            }

            ++startNode;
            FOREACHC(itc, vconfigs)
                path.insert(startNode, _treeForward._nodes.at(_treeForward.AddNode(-1,*itc)));
            // splice out in-between nodes in path
            path.erase(startNode, ++endNode);
            nrejected = 0;

            if( path.size() <= 2 )
                return;
        }
    }

protected:
    RobotBasePtr         _robot;

    std::vector<dReal>         _randomConfig;
    CollisionReportPtr _report;

    SpatialTree< RrtPlanner<Node>, Node > _treeForward;

    bool _bInit;

    inline boost::shared_ptr<RrtPlanner> shared_planner() { return boost::static_pointer_cast<RrtPlanner>(shared_from_this()); }
    inline boost::shared_ptr<RrtPlanner const> shared_planner_const() const { return boost::static_pointer_cast<RrtPlanner const>(shared_from_this()); }
};

class BirrtPlanner : public RrtPlanner<SimpleNode>
{
 public:
 BirrtPlanner(EnvironmentBasePtr penv) : RrtPlanner<SimpleNode>(penv)
    {
        __description = "Rosen's BiRRT planner";
    }
    virtual ~BirrtPlanner() {}

    virtual bool InitPlan(RobotBasePtr pbase, PlannerParametersConstPtr pparams)
    {
        EnvironmentMutex::scoped_lock lock(GetEnv()->GetMutex());
        if( !RrtPlanner<SimpleNode>::InitPlan(pbase,pparams) )
            return false;

        _bInit = false;
        RobotBase::RobotStateSaver savestate(_robot);

        _treeBackward.Reset(shared_planner(), _parameters.GetDOF());
        _treeBackward._fStepLength = _parameters._fStepLength;
        _treeBackward._distmetricfn = _parameters._distmetricfn;
    
        //read in all goals
        int goal_index = 0;
        int num_goals = 0;
        vector<dReal> vgoal(_parameters.GetDOF());

        while(goal_index < (int)_parameters.vgoalconfig.size()) {
            for(int i = 0 ; i < _parameters.GetDOF(); i++) {
                if(goal_index < (int)_parameters.vgoalconfig.size())
                    vgoal[i] = _parameters.vgoalconfig[goal_index];
                else {
                    RAVELOG_ERRORA("BirrtPlanner::InitPlan - Error: goals are improperly specified:\n");
                    return false;
                }
                goal_index++;
            }
        
            if(!CollisionFunctions::CheckCollision(GetParameters(),_robot,vgoal)) {

                bool bSuccess = true;     
                if( !!_parameters._constraintfn ) {
                    // filter
                    if( !_parameters._constraintfn(vgoal, vgoal, 0) ) {
                        // failed
                        RAVELOG_WARNA("goal state rejected by constraint fn\n");
                        bSuccess = false;
                    }
                }
            
                if( bSuccess ) {
                    // set up the goal states
                    _treeBackward.AddNode(-num_goals-1, vgoal);
                    num_goals++;
                }
            }
            else {
                RAVELOG_WARNA("goal in collision %s\n", _robot->CheckSelfCollision()?"(self)":NULL);
                if( GetEnv()->CheckCollision(KinBodyConstPtr(_robot), _report) ) {
                    RAVELOG_WARN(str(boost::format("birrt: robot initially in collision %s!\n")%_report->__str__()));
                }
            }
        }
    
        if( num_goals == 0 && !GetParameters()._samplegoalfn ) {
            RAVELOG_WARNA("no goals specified\n");
            return false;
        }    

        if( _parameters._nMaxIterations <= 0 )
            _parameters._nMaxIterations = 10000;
            
        _bInit = true;
        return true;
    }

    /// \param pOutStream returns which goal was chosen
    virtual bool PlanPath(TrajectoryBasePtr ptraj, boost::shared_ptr<std::ostream> pOutStream)
    {
        if(!_bInit) {
            RAVELOG_ERRORA("BirrtPlanner::PlanPath - Error, planner not initialized\n");
            return false;
        }
    
        EnvironmentMutex::scoped_lock lock(GetEnv()->GetMutex());
        uint32_t basetime = timeGetTime();

        // the main planning loop
        bool bConnected = false;
 
        RobotBase::RobotStateSaver savestate(_robot);

        SpatialTreeBase* TreeA = &_treeForward;
        SpatialTreeBase* TreeB = &_treeBackward;
        int iConnectedA, iConnectedB;
        int iter = 0;

        while(!bConnected && iter < 3*_parameters._nMaxIterations) {
            RAVELOG_VERBOSEA("iter: %d\n", iter);
            ++iter;

            if( !!_parameters._samplegoalfn ) {//  _treeBackward._nodes.size() == 0 ) {
                vector<dReal> vgoal;
                if( _parameters._samplegoalfn(vgoal) ) {
                    RAVELOG_VERBOSEA("found goal\n");
                    _treeBackward.AddNode(-10000,vgoal);
                }
            }

            if( !_parameters._samplefn(_randomConfig) )
                continue;
            
            // extend A
            ExtendType et = TreeA->Extend(_randomConfig, iConnectedA);

            // although check isn't necessary, having it improves running times
            if( et == ET_Failed ) {
                // necessary to increment iterator in case spaces are not connected
                if( iter > 3*_parameters._nMaxIterations ) {
                    RAVELOG_WARNA("iterations exceeded\n");
                    break;
                }
                continue;
            }

            // extend B toward A
            et = TreeB->Extend(TreeA->GetConfig(iConnectedA), iConnectedB);
            //GetEnv()->UpdatePublishedBodies();
            // if connected, success
            if( et == ET_Connected ) {
                bConnected = true;
                break;
            }

            swap(TreeA, TreeB);
            iter += 3;
            if( iter > 3*_parameters._nMaxIterations ) {
                RAVELOG_WARNA("iterations exceeded\n");
                break;
            }
        }
    
        if( !bConnected ) {
            RAVELOG_WARNA("plan failed, %fs\n",0.001f*(float)(timeGetTime()-basetime));
            return false;
        }
    
        list<SimpleNode*> vecnodes;
    
        // add nodes from the forward tree
        SimpleNode* pforward = _treeForward._nodes.at(TreeA == &_treeForward ? iConnectedA : iConnectedB);
        while(1) {
            vecnodes.push_front(pforward);
            if(pforward->parent < 0)
                break;
            pforward = _treeForward._nodes.at(pforward->parent);
        }

        // add nodes from the backward tree
        int goalindex = -1;

        SimpleNode *pbackward = _treeBackward._nodes.at(TreeA == &_treeBackward ? iConnectedA : iConnectedB);
        while(1) {
            vecnodes.push_back(pbackward);
            if(pbackward->parent < 0) {
                goalindex = -pbackward->parent-1;
                break;
            }
            pbackward = _treeBackward._nodes.at(pbackward->parent);
        }

        BOOST_ASSERT( goalindex >= 0 );
        if( pOutStream != NULL )
            *pOutStream << goalindex;

        _OptimizePath(vecnodes,NUM_OPT_ITERATIONS);
    
        Trajectory::TPOINT pt; pt.q.resize(_parameters.GetDOF());
    
        FOREACH(itnode, vecnodes) {
            for(int i = 0; i < _parameters.GetDOF(); ++i)
                pt.q[i] = (*itnode)->q[i];
            ptraj->AddPoint(pt);
        }

        RAVELOG_DEBUGA(str(boost::format("plan success, path=%d points in %fs\n")%ptraj->GetPoints().size()%(0.001f*(float)(timeGetTime()-basetime))));
    
        return true;
    }

    virtual PlannerParameters& GetParameters() { return _parameters; }

 protected:
    PlannerParameters _parameters;
    SpatialTree< RrtPlanner<SimpleNode>, SimpleNode > _treeBackward;
};

class BasicRrtPlanner : public RrtPlanner<SimpleNode>
{
 public:
 BasicRrtPlanner(EnvironmentBasePtr penv) : RrtPlanner<SimpleNode>(penv)
    {
        __description = "Rosen's BiRRT planner";
        _fGoalBiasProb = 0.05f;
        _bOneStep = true;
    }
    virtual ~BasicRrtPlanner() {}

    bool InitPlan(RobotBasePtr pbase, PlannerParametersConstPtr pparams)
    {
        EnvironmentMutex::scoped_lock lock(GetEnv()->GetMutex());
        if( !RrtPlanner<SimpleNode>::InitPlan(pbase,pparams) )
            return false;
        //_bOneStep = _parameters.vnParameters[0]>0;
    
        //read in all goals
        int goal_index = 0;
        vector<dReal> vgoal(_parameters.GetDOF());
        _vecGoals.resize(0);

        while(_parameters.vgoalconfig.size() > 0) {
            for(int i = 0 ; i < _parameters.GetDOF(); i++) {
                if(goal_index < (int)_parameters.vgoalconfig.size())
                    vgoal[i] = _parameters.vgoalconfig[goal_index];
                else {
                    RAVELOG_ERRORA("BirrtPlanner::InitPlan - Error: goals are improperly specified:\n");
                    return false;
                }
                goal_index++;
            }
        
            if(!CollisionFunctions::CheckCollision(GetParameters(),_robot,vgoal)) {

                bool bSuccess = true;     
                if( !!_parameters._constraintfn ) {
                    // filter
                    if( !_parameters._constraintfn(vgoal, vgoal, 0) ) {
                        // failed
                        RAVELOG_WARNA("goal state rejected by constraint fn\n");
                        bSuccess = false;
                    }
                }
            
                if( bSuccess ) {
                    // set up the goal states
                    _vecGoals.push_back(vgoal);
                }
            }
            else {
                RAVELOG_WARNA("goal in collision %s\n", _robot->CheckSelfCollision()?"(self)":NULL);
                if( GetEnv()->CheckCollision(KinBodyConstPtr(_robot), _report) ) {
                    RAVELOG_WARN(str(boost::format("birrt: robot initially in collision %s!\n")%_report->__str__()));
                }
            }
        
            if(goal_index == (int)_parameters.vgoalconfig.size())
                break;
        }
        
        if( _vecGoals.size() == 0 && !_parameters._goalfn ) {
            RAVELOG_WARNA("no goals or goal function specified\n");
            return false;
        }
        
        _bInit = true;
        RAVELOG_DEBUGA("RrtPlanner::InitPlan - RRT Planner Initialized\n");
        return true;
    }

    bool PlanPath(TrajectoryBasePtr ptraj, boost::shared_ptr<std::ostream> pOutStream)
    {
        if(!_bInit) {
            RAVELOG_WARNA("RrtPlanner::PlanPath - Error, planner not initialized\n");
            return false;
        }
    
        EnvironmentMutex::scoped_lock lock(GetEnv()->GetMutex());
        uint32_t basetime = timeGetTime();

        int lastnode = 0;    
        bool bSuccess = false;

        // the main planning loop 
        RobotBase::RobotStateSaver savestate(_robot);

        int iter = 0;
        int igoalindex = -1;

        while(!bSuccess && iter < _parameters._nMaxIterations) {
            iter++;

            if( !!_parameters._samplegoalfn ) {
                vector<dReal> vgoal;
                if( _parameters._samplegoalfn(vgoal) ) {
                    RAVELOG_VERBOSEA("found goal\n");
                    _vecGoals.push_back(vgoal);
                }
            }

            if( RaveRandomFloat() < _fGoalBiasProb && _vecGoals.size() > 0 )
                _randomConfig = _vecGoals[RaveRandomInt()%_vecGoals.size()];
            else if( !_parameters._samplefn(_randomConfig) )
                continue;

            // extend A
            ExtendType et = _treeForward.Extend(_randomConfig, lastnode, _bOneStep);

            if( et == ET_Connected ) {
                FOREACH(itgoal, _vecGoals) {
                    if( _treeForward._distmetricfn(*itgoal, _treeForward._nodes.at(lastnode)->q) < 2*_treeForward._fStepLength ) {
                        bSuccess = true;
                        igoalindex = (int)(itgoal-_vecGoals.begin());
                        break;
                    }
                }
            }

            // check the goal heuristic more often
            if( et != ET_Failed && !!_parameters._goalfn ) {
                if( _parameters._goalfn(_treeForward._nodes.at(lastnode)->q) <= 1e-4f ) {
                    bSuccess = true;
                    igoalindex = 0;
                    break;
                }
            }
        
            // check if reached any goals
            if( iter > _parameters._nMaxIterations ) {
                RAVELOG_WARNA("iterations exceeded %d\n", _parameters._nMaxIterations);
                break;
            }
        }
    
        if( !bSuccess ) {
            RAVELOG_DEBUGA("plan failed, %fs\n",0.001f*(float)(timeGetTime()-basetime));
            return false;
        }
    
        list<SimpleNode*> vecnodes;
    
        // add nodes from the forward tree
        SimpleNode* pforward = _treeForward._nodes.at(lastnode);
        while(1) {
            vecnodes.push_front(pforward);
            if(pforward->parent < 0)
                break;
            pforward = _treeForward._nodes.at(pforward->parent);
        }

        _OptimizePath(vecnodes,NUM_OPT_ITERATIONS);
    
        BOOST_ASSERT( igoalindex >= 0 );
        if( pOutStream != NULL )
            *pOutStream << igoalindex;

        Trajectory::TPOINT pt; pt.q.resize(_parameters.GetDOF());
        FOREACH(itnode, vecnodes) {
            for(int i = 0; i < _parameters.GetDOF(); ++i)
                pt.q[i] = (*itnode)->q[i];
            ptraj->AddPoint(pt);
        }

        RAVELOG_DEBUGA(str(boost::format("plan success, path=%d points in %fs\n")%ptraj->GetPoints().size()%((0.001f*(float)(timeGetTime()-basetime)))));
    
        return true;
    }

    virtual PlannerParameters& GetParameters() { return _parameters; }

 protected:
    PlannerParameters _parameters;
    float _fGoalBiasProb;
    bool _bOneStep;
    std::vector< std::vector<dReal> > _vecGoals;
};

class ExplorationPlanner : public RrtPlanner<SimpleNode>
{
 public:
 ExplorationPlanner(EnvironmentBasePtr penv) : RrtPlanner<SimpleNode>(penv)
    {
        __description = "RRT-based exploration planner";
    }
    virtual ~ExplorationPlanner() {}

    bool PlanPath(TrajectoryBasePtr ptraj, boost::shared_ptr<std::ostream> pOutStream)
    {
        if( !_bInit )
            return false;

        EnvironmentMutex::scoped_lock lock(GetEnv()->GetMutex());
        vector<dReal> vSampleConfig;

        RobotBase::RobotStateSaver saver(_robot);    
        int iter = 0;
        while(iter < _parameters._nMaxIterations && (int)_treeForward._nodes.size() < _parameters._nExpectedDataSize ) {
            ++iter;

            if( RaveRandomFloat() < _parameters._fExploreProb ) {
                // explore
                int inode = RaveRandomInt()%_treeForward._nodes.size();            
                SimpleNode* pnode = _treeForward._nodes.at(inode);

                if( !_parameters._sampleneighfn(vSampleConfig,pnode->q,_parameters._fStepLength) )
                    return false;

                if( !!_parameters._constraintfn ) {
                    _parameters._setstatefn(vSampleConfig);
                    if( !_parameters._constraintfn(pnode->q, vSampleConfig, 0) )
                        continue;
                }

                if( !CollisionFunctions::CheckCollision(GetParameters(),_robot,pnode->q, vSampleConfig, OPEN_START) ) {
                    _treeForward.AddNode(inode,vSampleConfig);
                    GetEnv()->UpdatePublishedBodies();
                    RAVELOG_DEBUGA(str(boost::format("size %d\n")%_treeForward._nodes.size()));
                }
            }
            else { // rrt extend
                if( !_parameters._samplefn(vSampleConfig) )
                    continue;
                int lastindex;
                if( _treeForward.Extend(vSampleConfig,lastindex,true) == ET_Connected ) {
                    RAVELOG_DEBUGA(str(boost::format("size %d\n")%_treeForward._nodes.size()));
                }
            }
        }
    
        // save nodes to trajectory
        FOREACH(itnode, _treeForward._nodes)
            ptraj->AddPoint(Trajectory::TPOINT((*itnode)->q,0));

        return true;
    }

    virtual PlannerParameters& GetParameters() { return _parameters; }
    
private:
    ExplorationParameters _parameters;
    
};

#endif
