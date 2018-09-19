#include "tile/codegen/access.h"

namespace vertexai {
namespace tile {
namespace codegen {

std::ostream& operator<<(std::ostream& stream, const BufferAccess& a) {
  stream << a.offset << ":"
         << "TODO";
  return stream;
}

std::ostream& operator<<(std::ostream& stream, const Index& a) {
  stream << a.name << ":" << a.range;
  return stream;
}

// Per index information
struct IdxInfo {
  // Is the index incomplete?  That is, does it derive from an index we are not tracking?
  bool incomplete = false;
  // What is the global value of the index (presuming it's not incomplete)
  std::vector<int64_t> gid;
};

struct Context {
  size_t idx_count = 0;     // How many indexes exist in this + parent contexts?
  bool exact = true;        // Are we still exact, or did we skip constraints based on incomplete indexes?
  Indexes indexes;          // The current set of indexes
  Constraints constraints;  // Current constraints
  std::map<std::string, IdxInfo> idx_info;     // Information on each current index by name
  std::map<std::string, BufferAccess> access;  // The information on each in scope buffer
};

void ComputeAccessRecursive(std::vector<AccessPattern>* out, const stripe::proto::Block& block, const Context& up) {
  // Prepare the new context
  Context self;
  // Update count of indexes
  self.idx_count = up.idx_count + block.idxs().size();
  // Copy across exactness
  self.exact = up.exact;
  // Copy across existing indexes
  self.indexes = up.indexes;
  // Copy across any constraints
  self.constraints = up.constraints;
  // First process all refinements
  // We make a templated lambda so we can work on both input/output refinements
  auto proc_refine = [&](const std::string& from, const std::string& into, const stripe::proto::BufferAccess& access) {
    // Check if source is in our parent
    auto it = up.access.find(from);
    // If not, don't don't bother
    if (it == up.access.end()) {
      return;
    }
    // Make a new access info for the 'into' name
    BufferAccess& ba = self.access[into];
    // Copy across from as a starting point
    ba = it->second;
    // Add the additional offset
    ba.offset += access.offset();
    // Add the additional strides
    for (int64_t s : access.strides()) {
      ba.strides.push_back(s);
    }
  };
  // Now we run it on ref_ins and ref_outs
  for (const auto& ref : block.ref_ins()) {
    proc_refine(ref.from(), ref.into(), ref.access());
  }
  for (const auto& ref : block.ref_outs()) {
    proc_refine(ref.from(), ref.into(), ref.access());
  }
  // If we have no refinements left, early return
  if (self.access.size() == 0) {
    return;
  }
  // Make index info for each index
  for (const auto& idx : block.idxs()) {
    // Get the id of this index
    size_t iid = self.indexes.size();
    // Add the index name + range
    self.indexes.push_back({idx.name(), idx.range(), idx.factor()});
    // Make a place to put the info on this index
    IdxInfo& info = self.idx_info[idx.name()];
    // Check for the index in the outer context
    auto upit = up.idx_info.find(idx.name());
    if (upit == up.idx_info.end()) {
      // This is a new index as far as we have record, check if it has a parent we didn't know about
      if (idx.factor() != 0) {
        // Yup, we can't know about the gid...
        info.incomplete = true;
      } else {
        // Gid is just outselved
        info.gid.resize(self.idx_count);
        info.gid[iid] = 1;
      }
    } else {
      const auto& old_info = upit->second;
      if (old_info.incomplete && idx.factor() != 0) {
        // If we derive from an incomplete index, we are also incomplete
        info.incomplete = true;
      } else {
        // Otherwise, multiply previous gid by factor and add self
        info.gid.resize(self.idx_count);
        for (size_t i = 0; i < old_info.gid.size(); i++) {
          info.gid[i] = idx.factor() * old_info.gid[i];
        }
        info.gid[iid] = 1;
      }
    }
  }
  // Next add all new constraints
  for (const auto& pcon : block.constraints()) {
    Constraint con;
    con.lhs.resize(self.idx_count);
    bool exact = true;
    for (size_t i = 0; i < size_t(pcon.lhs().size()); i++) {
      int64_t mul = pcon.lhs(i);
      const auto& info = self.idx_info[block.idxs(i).name()];
      if (info.incomplete && mul != 0) {
        exact = false;
        break;
      }
      const auto& gid = self.idx_info[block.idxs(i).name()].gid;
      for (size_t j = 0; j < gid.size(); j++) {
        con.lhs[j] += mul * gid[j];
      }
    }
    con.rhs = pcon.rhs();
    if (exact) {
      self.constraints.push_back(con);
    } else {
      self.exact = false;
    }
  }
  // Make another labmda to construct and add an access pattern
  auto add_access = [&](const std::string& name, bool is_write) {
    auto it = self.access.find(name);
    if (it == self.access.end()) {
      return;
    }
    const auto& a = it->second;
    out->emplace_back();
    AccessPattern& r = out->back();
    r.is_write = is_write;
    r.is_exact = self.exact;
    r.indexes = self.indexes;
    r.access = a;
    r.constraints = self.constraints;
  };
  // Now go over all statements (and possibly recurse)
  for (const auto& stmt : block.stmts()) {
    switch (stmt.op_case()) {
      case stripe::proto::Statement::kLoad:
        add_access(stmt.load().from(), false);
        break;
      case stripe::proto::Statement::kStore:
        add_access(stmt.store().from(), true);
        break;
      case stripe::proto::Statement::kBlock:
        // TODO: Consider prequalifying block (does it refine anything I care about) before descending
        ComputeAccessRecursive(out, stmt.block(), self);
        break;
      default:
        break;
    }
  }
}

std::ostream& operator<<(std::ostream& stream, const AccessPattern& ap) {
  stream << "Access Pattern: (is_write=" << ap.is_write << " exact=" << ap.is_exact << " offset=" << ap.access.offset
         << ") {\n";
  for (size_t i = 0; i < ap.indexes.size(); i++) {
    stream << "  " << ap.indexes[i].name << " range=" << ap.indexes[i].range << " stride=" << ap.access.strides[i]
           << "\n";
  }
  for (const auto& c : ap.constraints) {
    stream << "  ";
    for (const auto& v : c.lhs) {
      stream << v << " ";
    }
    stream << "< " << c.rhs << "\n";
  }
  stream << "}\n";
  return stream;
}

std::vector<AccessPattern> ComputeAccess(const stripe::proto::Block& block, const std::string& buffer) {
  std::vector<AccessPattern> out;
  Context top;
  top.access[buffer];  // Make an empty access pattern @ named location
  ComputeAccessRecursive(&out, block, top);
  return out;
}

}  // namespace codegen
}  // namespace tile
}  // namespace vertexai