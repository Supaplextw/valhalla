#include "baldr/graphreader.h"

#include <string>
#include <iostream>
#include <fstream>
#include <sys/stat.h>
#include <boost/filesystem.hpp>

#include "midgard/logging.h"
#include "midgard/sequence.h"

#include "baldr/connectivity_map.h"
using namespace valhalla::baldr;

namespace {
  constexpr size_t DEFAULT_MAX_CACHE_SIZE = 1073741824; //1 gig
  constexpr size_t AVERAGE_TILE_SIZE = 2097152; //2 megs
  constexpr size_t AVERAGE_MM_TILE_SIZE = 1024; //1k
}

namespace valhalla {
namespace baldr {

struct GraphReader::tile_extract_t : public midgard::tar {
  tile_extract_t(const boost::property_tree::ptree& pt):tar(pt.get<std::string>("tile_extract","")) {
    //if you really meant to load it
    if(pt.get_optional<std::string>("tile_extract")) {
      //map files to graph ids
      for(auto& c : contents) {
        try {
          auto id = GraphTile::GetTileId(c.first);
          tiles[id] = std::make_pair(const_cast<char*>(c.second.first), c.second.second);
        }
        catch(...){}
      }
      //couldn't load it
      if(tiles.empty()) {
        LOG_WARN("Tile extract could not be loaded");
      }//loaded ok but with possibly bad blocks
      else {
        LOG_INFO("Tile extract successfully loaded");
        if(corrupt_blocks)
          LOG_WARN("Tile extract had " + std::to_string(corrupt_blocks) + " corrupt blocks");
      }
    }
  }
  // TODO: dont remove constness, and actually make graphtile read only?
  std::unordered_map<uint64_t, std::pair<char*, size_t> > tiles;
};

std::shared_ptr<const GraphReader::tile_extract_t> GraphReader::get_extract_instance(const boost::property_tree::ptree& pt) {
  static std::shared_ptr<const GraphReader::tile_extract_t> tile_extract(new GraphReader::tile_extract_t(pt));
  return tile_extract;
}

// Constructor.
TileCache::TileCache(size_t max_size)
      : cache_size_(0), max_cache_size_(max_size)
{
}

// Reserves enough cache to hold (max_cache_size / tile_size) items.
void TileCache::Reserve(size_t tile_size)
{
  cache_.reserve(max_cache_size_ / tile_size);
}

// Checks if tile exists in the cache.
bool TileCache::Contains(const GraphId& graphid) const
{
  return cache_.find(graphid) != cache_.end();
}

// Lets you know if the cache is too large.
bool TileCache::OverCommitted() const
{
  return max_cache_size_ < cache_size_;
}

// Clears the cache.
void TileCache::Clear()
{
  cache_size_ = 0;
  cache_.clear();
}

// Get a pointer to a graph tile object given a GraphId.
const GraphTile* TileCache::Get(const GraphId& graphid) const
{
  auto cached = cache_.find(graphid);
  if(cached != cache_.end()) {
    return &cached->second;
  }
  return nullptr;
}

// Puts a copy of a tile of into the cache.
const GraphTile* TileCache::Put(const GraphId& graphid, const GraphTile& tile, size_t size)
{
  cache_size_ += size;
  return &cache_.emplace(graphid, tile).first->second;
}

// Constructor.
SynchronizedTileCache::SynchronizedTileCache(std::mutex& mutex, size_t max_size)
      : TileCache(max_size), mutex_ref_(mutex)
{
}

// Reserves enough cache to hold (max_cache_size / tile_size) items.
void SynchronizedTileCache::Reserve(size_t tile_size)
{
  std::lock_guard<std::mutex> lock(mutex_ref_);
  TileCache::Reserve(tile_size);
}

// Checks if tile exists in the cache.
bool SynchronizedTileCache::Contains(const GraphId& graphid) const
{
  std::lock_guard<std::mutex> lock(mutex_ref_);
  return TileCache::Contains(graphid);
}

// Lets you know if the cache is too large.
bool SynchronizedTileCache::OverCommitted() const
{
  std::lock_guard<std::mutex> lock(mutex_ref_);
  return TileCache::OverCommitted();
}

// Clears the cache.
void SynchronizedTileCache::Clear()
{
  std::lock_guard<std::mutex> lock(mutex_ref_);
  TileCache::Clear();
}

// Get a pointer to a graph tile object given a GraphId.
const GraphTile* SynchronizedTileCache::Get(const GraphId& graphid) const
{
  std::lock_guard<std::mutex> lock(mutex_ref_);
  return TileCache::Get(graphid);
}

// Puts a copy of a tile of into the cache.
const GraphTile* SynchronizedTileCache::Put(const GraphId& graphid, const GraphTile& tile, size_t size)
{
  std::lock_guard<std::mutex> lock(mutex_ref_);
  return TileCache::Put(graphid, tile, size);
}

// Puts a copy of a tile of into the cache without locking.
const GraphTile* SynchronizedTileCache::PutNoLock(const GraphId& graphid, const GraphTile& tile, size_t size)
{
  return TileCache::Put(graphid, tile, size);
}

// Constructor.
CopyForwardingTileCache::CopyForwardingTileCache(size_t max_size)
      : SynchronizedTileCache(mutex_, max_size)
{
  std::lock_guard<std::mutex> lock(mutex_);
  members_.insert(this);
  LOG_DEBUG("CopyForwardingTileCache(): " + std::to_string(members_.size()) + " members");
}

// Destructor.
CopyForwardingTileCache::~CopyForwardingTileCache()
{
  std::lock_guard<std::mutex> lock(mutex_);
  members_.erase(this);
  LOG_DEBUG("~CopyForwardingTileCache(): " + std::to_string(members_.size()) + " members");
}

// Puts a copy of a tile of into all caches.
const GraphTile* CopyForwardingTileCache::Put(const GraphId& graphid, const GraphTile& tile, size_t size)
{
  std::lock_guard<std::mutex> lock(mutex_);
  // Put into current cache
  const GraphTile* result = PutNoLock(graphid, tile, size);
  // Put into neighbor's caches
  for (auto * cache : members_)
    if (cache != this)
        cache->PutNoLock(graphid, tile, size);

  return result;
}

std::mutex CopyForwardingTileCache::mutex_;
std::unordered_set<CopyForwardingTileCache*> CopyForwardingTileCache::members_;

// Constructs tile cache.
TileCache* TileCacheFactory::createTileCache(const boost::property_tree::ptree& pt)
{
  size_t max_cache_size = pt.get<size_t>("max_cache_size", DEFAULT_MAX_CACHE_SIZE);

  // copy forwarding cache optimized for memory use
  if (pt.get<bool>("memory_optimized_cache", false))
    return new CopyForwardingTileCache(max_cache_size);

  // default
  return new TileCache(max_cache_size);
}

// Constructor using separate tile files
GraphReader::GraphReader(const boost::property_tree::ptree& pt)
    : tile_hierarchy_(pt.get<std::string>("tile_dir")),
      tile_extract_(get_extract_instance(pt)),
      cache_(TileCacheFactory::createTileCache(pt)) {
  // Reserve cache (based on whether using individual tile files or shared,
  // mmap'd file
  cache_->Reserve(tile_extract_->tiles.empty() ? AVERAGE_TILE_SIZE : AVERAGE_MM_TILE_SIZE);
}

// Method to test if tile exists
bool GraphReader::DoesTileExist(const GraphId& graphid) const {
  //if you are using an extract only check that
  if(!tile_extract_->tiles.empty())
    return tile_extract_->tiles.find(graphid) != tile_extract_->tiles.cend();
  //otherwise check memory or disk
  if(cache_->Contains(graphid))
    return true;
  std::string file_location = tile_hierarchy_.tile_dir() + "/" +
    GraphTile::FileSuffix(graphid.Tile_Base(), tile_hierarchy_);
  struct stat buffer;
  return stat(file_location.c_str(), &buffer) == 0;
}
bool GraphReader::DoesTileExist(const boost::property_tree::ptree& pt, const GraphId& graphid) {
  //if you are using an extract only check that
  auto extract = get_extract_instance(pt);
  if(!extract->tiles.empty())
    return extract->tiles.find(graphid) != extract->tiles.cend();
  //otherwise check the disk
  TileHierarchy tile_hierarchy(pt.get<std::string>("tile_dir"));
  std::string file_location = tile_hierarchy.tile_dir() + "/" +
    GraphTile::FileSuffix(graphid.Tile_Base(), tile_hierarchy);
  struct stat buffer;
  return stat(file_location.c_str(), &buffer) == 0;
}

// Get a pointer to a graph tile object given a GraphId. Return nullptr
// if the tile is not found/empty
const GraphTile* GraphReader::GetGraphTile(const GraphId& graphid) {
  //TODO: clear the cache automatically once we become overcommitted by a certain amount

  // Return nullptr if not a valid tile
  if (!graphid.Is_Valid()) {
    return nullptr;
  }

  // Check if the level/tileid combination is in the cache
  auto base = graphid.Tile_Base();
  if(auto cached = cache_->Get(base)) {
    return cached;
  }

  // Try getting it from the memmapped tar extract
  if (!tile_extract_->tiles.empty()) {
    // Do we have this tile
    auto t = tile_extract_->tiles.find(base);
    if(t == tile_extract_->tiles.cend())
      return nullptr;

    // This initializes the tile from mmap
    GraphTile tile(base, t->second.first, t->second.second);
    if (!tile.header())
      return nullptr;

    // Keep a copy in the cache and return it
    size_t size = AVERAGE_MM_TILE_SIZE; // tile.end_offset();  // TODO what size??
    auto inserted = cache_->Put(base, tile, size);
    return inserted;
  }// Try getting it from flat file
  else {
    // This reads the tile from disk
    GraphTile tile(tile_hierarchy_, base);
    if (!tile.header())
      return nullptr;

    // Keep a copy in the cache and return it
    size_t size = tile.header()->end_offset();
    auto inserted = cache_->Put(base, tile, size);
    return inserted;
  }
}

const GraphTile* GraphReader::GetGraphTile(const GraphId& graphid, const GraphTile*& tile) {
  if(!tile || tile->id() != graphid.Tile_Base())
    tile = GetGraphTile(graphid);
  return tile;
}

const GraphTile* GraphReader::GetGraphTile(const PointLL& pointll, const uint8_t level){
  GraphId id = tile_hierarchy_.GetGraphId(pointll, level);
  return (id.Is_Valid()) ? GetGraphTile(tile_hierarchy_.GetGraphId(pointll, level)) : nullptr;
}

const GraphTile* GraphReader::GetGraphTile(const PointLL& pointll){
  return GetGraphTile(pointll, tile_hierarchy_.levels().rbegin()->second.level);
}

const TileHierarchy& GraphReader::GetTileHierarchy() const {
  return tile_hierarchy_;
}

// Clears the cache
void GraphReader::Clear() {
  cache_->Clear();
}

// Returns true if the cache is over committed with respect to the limit
bool GraphReader::OverCommitted() const {
  return cache_->OverCommitted();
}

// Convenience method to get an opposing directed edge graph Id.
GraphId GraphReader::GetOpposingEdgeId(const GraphId& edgeid) {
  const GraphTile* NO_TILE = nullptr;
  return GetOpposingEdgeId(edgeid, NO_TILE);
}
GraphId GraphReader::GetOpposingEdgeId(const GraphId& edgeid, const GraphTile*& tile) {
  tile = GetGraphTile(edgeid);
  if(!tile)
    return {};
  const auto* directededge = tile->directededge(edgeid);

  // For now return an invalid Id if this is a transit edge
  if (directededge->IsTransitLine()) {
    return {};
  }

  // Get the opposing edge, if edge leaves the tile get the end node's tile
  GraphId id = directededge->endnode();

  if (directededge->leaves_tile()) {
    // Get tile at the end node
    tile = GetGraphTile(id);
  }

  if (tile != nullptr) {
    id.fields.id = tile->node(id)->edge_index() + directededge->opp_index();
    return id;
  } else {
    LOG_ERROR("Invalid tile for opposing edge: tile ID= " + std::to_string(id.tileid()) + " level= " + std::to_string(id.level()));
    if (directededge->trans_up() || directededge->trans_down()) {
      LOG_ERROR("transition edge being checked?");
    }
  }
  return {};
}

// Convenience method to get an opposing directed edge.
const DirectedEdge* GraphReader::GetOpposingEdge(const GraphId& edgeid) {
  const GraphTile* NO_TILE = nullptr;
  return GetOpposingEdge(edgeid, NO_TILE);
}
const DirectedEdge* GraphReader::GetOpposingEdge(const GraphId& edgeid, const GraphTile*& tile) {
  GraphId oppedgeid = GetOpposingEdgeId(edgeid, tile);
  return oppedgeid.Is_Valid() ? tile->directededge(oppedgeid) : nullptr;
}

// Convenience method to determine if 2 directed edges are connected.
bool GraphReader::AreEdgesConnected(const GraphId& edge1, const GraphId& edge2) {
  // Check if there is a transition edge between n1 and n2
  auto is_transition = [this](const GraphId& n1, const GraphId& n2) {
    if (n1.level() == n2.level()) {
      return false;
    } else {
      uint32_t n = 0, id = 0;
      const DirectedEdge* de = GetGraphTile(n1)->GetDirectedEdges(n1.id(), n, id);
      for (uint32_t i = 0; i < n; i++, de++) {
        if ((de->trans_up() || de->trans_down()) && de->endnode() == n2) {
          return true;
        }
      }
    }
    return false;
  };

  // Get both directed edges
  const GraphTile* t1 = GetGraphTile(edge1);
  const DirectedEdge* de1 = t1->directededge(edge1);
  const GraphTile* t2 = (edge2.Tile_Base() == edge1.Tile_Base()) ?
                          t1 : GetGraphTile(edge2);
  const DirectedEdge* de2 = t2->directededge(edge2);
  if (de1->endnode() == de2->endnode() ||
      is_transition(de1->endnode(), de2->endnode())) {
    return true;
  }

  // Get opposing edge to de1
  const DirectedEdge* de1_opp = GetOpposingEdge(edge1, t1);
  if (de1_opp->endnode() == de2->endnode() ||
      is_transition(de1_opp->endnode(), de2->endnode())) {
    return true;
  }

  // Get opposing edge to de2 and compare to both edge1 endnodes
  const DirectedEdge* de2_opp = GetOpposingEdge(edge2, t2);
  if (de2_opp->endnode() == de1->endnode() ||
      de2_opp->endnode() == de1_opp->endnode() ||
      is_transition(de2_opp->endnode(), de1->endnode()) ||
      is_transition(de2_opp->endnode(), de1_opp->endnode())) {
    return true;
  }
  return false;
}

// Get the shortcut edge that includes this edge.
GraphId GraphReader::GetShortcut(const GraphId& id) {
  // Lambda to get continuing edge at a node. Skips the specified edge Id
  // transition edges, shortcut edges, and transit connections. Returns
  // nullptr if more than one edge remains or no continuing edge is found.
  auto continuing_edge = [](const GraphTile* tile, const GraphId& edgeid,
                            const NodeInfo* nodeinfo) {
    uint32_t idx = nodeinfo->edge_index();
    const DirectedEdge* continuing_edge = static_cast<const DirectedEdge*>(nullptr);
    const DirectedEdge* directededge = tile->directededge(idx);
    for (uint32_t i = 0; i < nodeinfo->edge_count(); i++, directededge++, idx++) {
      if (directededge->trans_up() || directededge->trans_down() ||
          idx == edgeid.id() || directededge->is_shortcut() ||
          directededge->use() == Use::kTransitConnection) {
        continue;
      }
      if (continuing_edge != nullptr) {
        return static_cast<const DirectedEdge*>(nullptr);
      }
      continuing_edge = directededge;
    }
    return continuing_edge;
  };

  // No shortcuts on the local level or transit level.
  if (id.level() >= tile_hierarchy_.levels().rbegin()->second.level) {
    return { };
  }

  // If this edge is a shortcut return this edge Id
  const GraphTile* tile = GetGraphTile(id);
  const DirectedEdge* directededge = tile->directededge(id);
  if (directededge->is_shortcut()) {
    return id;
  }

  // Walk backwards along the opposing directed edge until a shortcut
  // beginning is found or to get the continuing edge until a node that starts
  // the shortcut is found or there are 2 or more other regular edges at the
  // node.
  GraphId edgeid = id;
  const NodeInfo* node = nullptr;
  const DirectedEdge* cont_de = nullptr;
  while (true) {
    // Get the continuing directed edge. Initial case is to use the opposing
    // directed edge.
    cont_de = (node == nullptr) ? GetOpposingEdge(id) :
                continuing_edge(tile, edgeid, node);
    if (cont_de == nullptr) {
      return { };
    }

    // Get the end node and end node tile
    GraphId endnode = cont_de->endnode();
    if (cont_de->leaves_tile()) {
      tile = GetGraphTile(endnode.Tile_Base());
    }
    node = tile->node(endnode);

    // Get the opposing edge Id and its directed edge
    uint32_t idx = node->edge_index() + cont_de->opp_index();
    edgeid = { endnode.tileid(), endnode.level(), idx };
    directededge = tile->directededge(edgeid);
    if (directededge->superseded()) {
      // Get the shortcut edge Id that supersedes this edge
      uint32_t idx = node->edge_index() + (directededge->superseded() - 1);
      return GraphId(endnode.tileid(), endnode.level(), idx);
    }
  }
  return { };
}

// Convenience method to get the relative edge density (from the
// begin node of an edge).
uint32_t GraphReader::GetEdgeDensity(const GraphId& edgeid) {
  // Get the end node of the opposing directed edge
  const DirectedEdge* opp_edge = GetOpposingEdge(edgeid);
  GraphId id = opp_edge->endnode();
  const GraphTile* tile = GetGraphTile(id);
  return (tile != nullptr) ? tile->node(id)->density() : 0;
}


std::unordered_set<GraphId> GraphReader::GetTileSet() const {
  //either mmap'd tiles
  std::unordered_set<GraphId> tiles;
  if(tile_extract_->tiles.size()) {
    for(const auto& t : tile_extract_->tiles)
      tiles.emplace(t.first);
  }//or individually on disk
  else {
    //for each level
    for(uint8_t level = 0; level < tile_hierarchy_.levels().rbegin()->first + 1; ++level) {
      //crack open this level of tiles directory
      boost::filesystem::path root_dir(tile_hierarchy_.tile_dir() + '/' + std::to_string(level) + '/');
      if(boost::filesystem::exists(root_dir) && boost::filesystem::is_directory(root_dir)) {
        //iterate over all the files in there
        for (boost::filesystem::recursive_directory_iterator i(root_dir), end; i != end; ++i) {
          if (!boost::filesystem::is_directory(i->path())) {
            //add it if it can be parsed as a valid tile file name
            try { tiles.emplace(GraphTile::GetTileId(i->path().string())); }
            catch (...) { }
          }
        }
      }
    }
  }

  //give them back
  return tiles;
}

}
}
