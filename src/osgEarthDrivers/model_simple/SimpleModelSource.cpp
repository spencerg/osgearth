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

#include "SimpleModelOptions"
#include <osgEarth/ModelSource>
#include <osgEarth/Registry>
#include <osgEarth/Map>
#include <osgEarth/FileUtils>
#include <osg/Notify>
#include <osgDB/FileNameUtils>

using namespace osgEarth;
using namespace osgEarth::Drivers;

#include <osgEarth/HTTPClient>

class SimpleModelSource : public ModelSource
{
public:
    SimpleModelSource( const ModelSourceOptions& options )
        : ModelSource( options ), _options(options) { }

    //override
    void initialize( const osgDB::Options* dbOptions, const osgEarth::Map* map )
    {
        ModelSource::initialize( dbOptions, map );
    }

    // override
    osg::Node* createNode( ProgressCallback* progress )
    {
        osg::ref_ptr<osg::Node> result;

        // required if the model includes local refs, like PagedLOD or ProxyNode:
        osg::ref_ptr<osgDB::Options> localOptions = _dbOptions.get() ? new osgDB::Options(*_dbOptions.get()) : new osgDB::Options();
        localOptions->getDatabasePathList().push_back( osgDB::getFilePath(_options.url()->full()) );

        return _options.url()->readNode( localOptions.get(), CachePolicy::NO_CACHE, progress ).releaseNode();
    }

protected:

    const SimpleModelOptions           _options;
    const osg::ref_ptr<osgDB::Options> _dbOptions;
};


class SimpleModelSourceFactory : public ModelSourceDriver
{
public:
    SimpleModelSourceFactory()
    {
        supportsExtension( "osgearth_model_simple", "osgEarth simple model plugin" );
    }

    virtual const char* className()
    {
        return "osgEarth Simple Model Plugin";
    }

    virtual ReadResult readObject(const std::string& file_name, const Options* options) const
    {
        if ( !acceptsExtension(osgDB::getLowerCaseFileExtension( file_name )))
            return ReadResult::FILE_NOT_HANDLED;

        return ReadResult( new SimpleModelSource( getModelSourceOptions(options) ) );
    }
};

REGISTER_OSGPLUGIN(osgearth_model_simple, SimpleModelSourceFactory) 
