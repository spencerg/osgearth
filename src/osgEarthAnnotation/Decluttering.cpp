/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
* Copyright 2008-2010 Pelican Mapping
* http://osgearth.org
*
* osgEarth is free software; you can redistribute it and/or modify
* it under the terms of the GNU Lesser General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>
*/
#include <osgEarthAnnotation/Decluttering>
#include <osgEarth/Utils>
#include <osgUtil/RenderBin>
#include <set>
#include <algorithm>

#define LC "[Declutter] "

using namespace osgEarth;
using namespace osgEarth::Annotation;

//----------------------------------------------------------------------------

namespace
{
    // wrapper to satisfy the template processor..
    struct SortContainer
    {
        SortContainer( DeclutterPriorityFunctor& f ) : _f(f) { }
        const DeclutterPriorityFunctor& _f;
        bool operator()( const osgUtil::RenderLeaf* lhs, const osgUtil::RenderLeaf* rhs ) const 
        {
            return _f(lhs, rhs);
        }
    };
}

//----------------------------------------------------------------------------

/**
 * A custom RenderLeaf sorting algorithm for decluttering objects.
 * It first sorts front-to-back so that objects closer to the camera get
 * priority. (You can replace this sorting algorithm with your own.)
 * Then it goes through the drawables and removes any that try to occupy
 * already occupied space (in eye space). Drawables with a common parent
 * node (e.g., under the same Geode) are processed as a group.
 *
 * We can easily modify this to do other interesting things, like shift
 * objects around or scale them based on occlusion.
 *
 * To submit an object for decluttering, all you have to do is call
 * obj->getStateSet()->setRenderBinDetails( binNum, OSGEARTH_DECLUTTER_BIN );
 */
struct /*internal*/ DeclutterSort : public osgUtil::RenderBin::SortCallback
{
    DeclutterPriorityFunctor* _f;

    /**
     * Constructs the new sorter.
     * @param f Custom declutter sorting predicate. Pass NULL to use the 
     *          default sorter (sort by distance-to-camera).
     */
    DeclutterSort( DeclutterPriorityFunctor* f = 0L ) : _f(f)
    {
        //nop
    }

    // override
    void sortImplementation(osgUtil::RenderBin* bin)
    {
        osgUtil::RenderBin::RenderLeafList& leaves = bin->getRenderLeafList();

        // first, sort the leaves.
        if ( _f )
        {
            // if there's a custom sorting function installed
            bin->copyLeavesFromStateGraphListToRenderLeafList();
            std::sort( leaves.begin(), leaves.end(), SortContainer( *_f ) );
        }
        else
        {
            // default behavior is to sort by depth.
            bin->sortFrontToBack();
        }

        // nothing to sort? bail out
        if ( leaves.size() == 0 )
            return;
        
        // List of render leaves that pass the initial visibility test
        osgUtil::RenderBin::RenderLeafList passed;
        passed.reserve( leaves.size() );

        // list of occlusion boxes - pairs the drawable's parent with the bounding box.
        typedef std::pair<const osg::Node*, osg::BoundingBox> RenderLeafBox;
        std::vector<RenderLeafBox> used;

        // compute a window matrix so we can do window-space culling:
        const osg::Viewport* vp = bin->getStage()->getCamera()->getViewport();
        osg::Matrix windowMatrix = vp->computeWindowMatrix();

        // Track the parent nodes of drawables that are obscured (and culled). Drawables
        // with the same parent node (typically a Geode) are considered to be grouped and
        // will be culled as a group.
        std::set<const osg::Node*> culledParents;

        // Go through each leaf and test for visibility.
        for( osgUtil::RenderBin::RenderLeafList::iterator i = leaves.begin(); i != leaves.end(); ++i )
        {
            bool visible = true;

            osgUtil::RenderLeaf* leaf = *i;
            const osg::Drawable* drawable = leaf->getDrawable();
            const osg::Node* drawableParent = drawable->getParent(0);

            // if this leaf is already in a culled group, skip it.
            if ( culledParents.find(drawableParent) != culledParents.end() )
            {
                continue;
            }

            // transform the bounding box of the drawable into window-space.
            osg::BoundingBox box = drawable->getBound();
            static osg::Vec4d s_zero_w(0,0,0,1);
            osg::Vec4d clip = s_zero_w * (*leaf->_modelview.get()) * (*leaf->_projection.get());
            osg::Vec3d clip_ndc( clip.x()/clip.w(), clip.y()/clip.w(), clip.z()/clip.w() );
            osg::Vec3f winPos = clip_ndc * windowMatrix;
            osg::Vec2f offset( -box.xMin(), -box.yMin() );
            box.set(
                winPos.x() + box.xMin(),
                winPos.y() + box.yMin(),
                winPos.z(),
                winPos.x() + box.xMax(),
                winPos.y() + box.yMax(),
                winPos.z() );

            // weed out any drawables that are obscured by closer drawables.
            // TODO: think about a more efficient algorithm - right now we are just using
            // brute force to compare all bbox's
            for( std::vector<RenderLeafBox>::const_iterator j = used.begin(); j != used.end(); ++j )
            {
                // only need a 2D test since we're in clip space
                bool isClear =
                    box.xMin() > j->second.xMax() ||
                    box.xMax() < j->second.xMin() ||
                    box.yMin() > j->second.yMax() ||
                    box.yMax() < j->second.yMin();

                // if there's an overlap (and the conflict isn't from the same drawable
                // parent, which is acceptable), then the leaf is culled.
                if ( !isClear && drawableParent != j->first )
                {
                    visible = false;
                    break;
                }
            }

            if ( visible )
            {
                // passed the test, so add the leaf's bbox to the "used" list, and add the leaf
                // to the final draw list.
                used.push_back( std::make_pair(drawableParent, box) );
                passed.push_back( leaf );

                // modify the leaf's modelview matrix to correctly position it in the 2D ortho
                // projection when it's drawn later. (Note: we need a new RefMatrix since the
                // original might be shared ... potential optimization here)
                leaf->_modelview = new osg::RefMatrix( osg::Matrix::translate(
                    box.xMin() + offset.x(),
                    box.yMin() + offset.y(), 
                    0) );
            }
            else
            {
                // culled, so put the parent in the parents list so that any future leaves
                // with the same parent will be trivially rejected
                culledParents.insert( drawable->getParent(0) );
            }
        }

        // copy the final draw list back into the bin, rejecting any leaves whose parents
        // are in the cull list.
        leaves.clear();
        for( osgUtil::RenderBin::RenderLeafList::const_iterator i=passed.begin(); i != passed.end(); ++i )
        {
            osgUtil::RenderLeaf* leaf = *i;
            if ( culledParents.find( leaf->getDrawable()->getParent(0) ) == culledParents.end() )
            {
                leaves.push_back( leaf );
            }
        }
    }
};

/**
 * Custom draw routine for our declutter render bin.
 */
struct DeclutterDraw : public osgUtil::RenderBin::DrawCallback
{
    /**
     * Draws a bin. Most of this code is copied from osgUtil::RenderBin::drawImplementation.
     * The modifications are (a) skipping code to render child bins, (b) setting a bin-global
     * projection matrix in orthographic space, and (c) calling our custom "renderLeaf()" method 
     * instead of RenderLeaf::render()
     */
    void drawImplementation( osgUtil::RenderBin* bin, osg::RenderInfo& renderInfo, osgUtil::RenderLeaf*& previous )
    {
        osg::State& state = *renderInfo.getState();

        unsigned int numToPop = (previous ? osgUtil::StateGraph::numToPop(previous->_parent) : 0);
        if (numToPop>1) --numToPop;
        unsigned int insertStateSetPosition = state.getStateSetStackSize() - numToPop;

        if (bin->getStateSet())
        {
            state.insertStateSet(insertStateSetPosition, bin->getStateSet());
        }

        /* note: removed code that draws child bins (unsupported) */

        // apply a window-space projection matrix.
        const osg::Viewport* vp = renderInfo.getCurrentCamera()->getViewport();
        if ( vp )
        {
            osg::ref_ptr<osg::RefMatrix> rm = new osg::RefMatrix( osg::Matrix::ortho2D(
                vp->x(), vp->x()+vp->width()-1,
                vp->y(), vp->y()+vp->height()-1 ) );
            state.applyProjectionMatrix( rm.get() );
        }

        // draw fine grained ordering.
        osgUtil::RenderBin::RenderLeafList& leaves = bin->getRenderLeafList();
        for(osgUtil::RenderBin::RenderLeafList::iterator rlitr = leaves.begin();
            rlitr!= leaves.end();
            ++rlitr)
        {
            osgUtil::RenderLeaf* rl = *rlitr;
            renderLeaf( rl, renderInfo, previous );
            previous = rl;
        }

        if ( bin->getStateSet() )
        {
            state.removeStateSet(insertStateSetPosition);
        }
    }

    /**
     * Renders a single leaf. We already applied the projection matrix, so here we only
     * need to apply a modelview matrix that specifies the ortho offset of the drawable.
     *
     * Most of this code is copied from RenderLeaf::draw() -- but I removed all the code
     * dealing with nested bins, since decluttering does not support them.
     */
    void renderLeaf( osgUtil::RenderLeaf* leaf, osg::RenderInfo& renderInfo, osgUtil::RenderLeaf*& previous )
    {
        osg::State& state = *renderInfo.getState();

        // don't draw this leaf if the abort rendering flag has been set.
        if (state.getAbortRendering())
        {
            //cout << "early abort"<<endl;
            return;
        }

        state.applyModelViewMatrix( leaf->_modelview.get() );

        if (previous)
        {
            // apply state if required.
            osgUtil::StateGraph* prev_rg = previous->_parent;
            osgUtil::StateGraph* prev_rg_parent = prev_rg->_parent;
            osgUtil::StateGraph* rg = leaf->_parent;
            if (prev_rg_parent!=rg->_parent)
            {
                osgUtil::StateGraph::moveStateGraph(state,prev_rg_parent,rg->_parent);

                // send state changes and matrix changes to OpenGL.
                state.apply(rg->getStateSet());

            }
            else if (rg!=prev_rg)
            {
                // send state changes and matrix changes to OpenGL.
                state.apply(rg->getStateSet());
            }
        }
        else
        {
            // apply state if required.
            osgUtil::StateGraph::moveStateGraph(state,NULL,leaf->_parent->_parent);

            state.apply(leaf->_parent->getStateSet());
        }

        // if we are using osg::Program which requires OSG's generated uniforms to track
        // modelview and projection matrices then apply them now.
        if (state.getUseModelViewAndProjectionUniforms()) 
            state.applyModelViewAndProjectionUniformsIfRequired();

        // draw the drawable
        leaf->_drawable->draw(renderInfo);
        
        if (leaf->_dynamic)
        {
            state.decrementDynamicObjectCount();
        }
    }
};

//----------------------------------------------------------------------------

/**
 * The actual custom render bin
 * This wants to be in the global scope for the dynamic registration to work,
 * hence the annoyinging long class name
 */
class osgEarthAnnotationDeclutterRenderBin : public osgUtil::RenderBin
{
public:
    static const std::string BIN_NAME;

    osgEarthAnnotationDeclutterRenderBin()
    {
        this->setName( BIN_NAME );
        clearSortingFunctor();
        setDrawCallback( new DeclutterDraw() );
    }

    void setSortingFunctor( DeclutterPriorityFunctor* f )
    {
        _f = f;
        setSortCallback( new DeclutterSort(f) );
    }

    void clearSortingFunctor()
    {
        setSortCallback( new DeclutterSort() );
    }

protected:
    osg::ref_ptr<DeclutterPriorityFunctor> _f;
};
const std::string osgEarthAnnotationDeclutterRenderBin::BIN_NAME = OSGEARTH_DECLUTTER_BIN;

//----------------------------------------------------------------------------

//static
void
Decluttering::setDeclutterPriorityFunctor( DeclutterPriorityFunctor* functor )
{
    // pull our prototype
    osgEarthAnnotationDeclutterRenderBin* bin = dynamic_cast<osgEarthAnnotationDeclutterRenderBin*>(
        osgUtil::RenderBin::getRenderBinPrototype( OSGEARTH_DECLUTTER_BIN ) );

    if ( bin )
    {
        bin->setSortingFunctor( functor );
    }
}

void
Decluttering::clearDeclutterPriorityFunctor()
{
    // pull our prototype
    osgEarthAnnotationDeclutterRenderBin* bin = dynamic_cast<osgEarthAnnotationDeclutterRenderBin*>(
        osgUtil::RenderBin::getRenderBinPrototype( OSGEARTH_DECLUTTER_BIN ) );

    if ( bin )
    {
        bin->clearSortingFunctor();
    }
}

//----------------------------------------------------------------------------

/** the actual registration. */
extern "C" void osgEarth_declutter(void) {}
static osgEarthAnnotationRegisterRenderBinProxy<osgEarthAnnotationDeclutterRenderBin> s_regbin(
    osgEarthAnnotationDeclutterRenderBin::BIN_NAME);
