#pragma once
#include <json.hpp>
#include <cstdint>
#include <deque>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>

namespace asteria_pipeline_diagnostic
{
using Json = nlohmann::json;
inline constexpr const char *Author = "Anokianx";

class Cache
{
    size_t historyCapacity_, selectedCapacity_;
    std::unordered_map<std::string,Json> history_, selected_;
    std::deque<std::string> historyOrder_, selectedOrder_;
    uint64_t putRequests_=0, created_=0, duplicatePuts_=0, historyEvictions_=0, historyCapacityDrops_=0;
    uint64_t selectionRequests_=0, selectedTotal_=0, duplicateSelections_=0, selectedEvictions_=0, missing_=0, selectedCapacityDrops_=0;
    uint64_t selectionRevision_=0, revisionDrops_=0;
    bool counterOverflow_=false, revisionExhausted_=false;

    void Increment(uint64_t &value)
    {
        if (value==std::numeric_limits<uint64_t>::max()) counterOverflow_=true;
        else ++value;
    }

    static void Insert(std::unordered_map<std::string,Json> &records,std::deque<std::string> &order,
                       const std::string &key,const Json &record)
    {
        auto inserted=records.emplace(key,record);
        try { order.push_back(key); }
        catch (...) { records.erase(inserted.first); throw; }
    }

public:
    explicit Cache(size_t historyCapacity=4096,size_t selectedCapacity=256)
        : historyCapacity_(historyCapacity),selectedCapacity_(selectedCapacity) {}

    bool Put(const std::string &key,const Json &record)
    {
        Increment(putRequests_);
        if (history_.find(key)!=history_.end() || selected_.find(key)!=selected_.end())
        {
            Increment(duplicatePuts_);
            return false;
        }
        if (!historyCapacity_)
        {
            Increment(historyCapacityDrops_);
            return false;
        }
        Insert(history_,historyOrder_,key,record);
        Increment(created_);
        if (history_.size()>historyCapacity_)
        {
            history_.erase(historyOrder_.front());
            historyOrder_.pop_front();
            Increment(historyEvictions_);
        }
        return true;
    }

    bool Select(const std::string &key)
    {
        Increment(selectionRequests_);
        if (selected_.find(key)!=selected_.end())
        {
            Increment(duplicateSelections_);
            return true;
        }
        const auto found=history_.find(key);
        if (found==history_.end())
        {
            Increment(missing_);
            return false;
        }
        if (!selectedCapacity_)
        {
            Increment(selectedCapacityDrops_);
            return false;
        }
        if (selectionRevision_==std::numeric_limits<uint64_t>::max())
        {
            revisionExhausted_=true;
            Increment(revisionDrops_);
            return false;
        }
        Insert(selected_,selectedOrder_,key,found->second);
        Increment(selectedTotal_);
        if (selected_.size()>selectedCapacity_)
        {
            selected_.erase(selectedOrder_.front());
            selectedOrder_.pop_front();
            Increment(selectedEvictions_);
        }
        ++selectionRevision_;
        return true;
    }

    uint64_t SelectionRevision() const { return selectionRevision_; }

    const Json *Peek(const std::string &key) const
    {
        const auto historical=history_.find(key);
        if (historical!=history_.end()) return &historical->second;
        const auto selected=selected_.find(key);
        return selected==selected_.end()?nullptr:&selected->second;
    }

    Json Stats() const
    {
        return {{"author",Author},{"scope","native_create_observer_only"},
            {"capacity",historyCapacity_},{"history_capacity",historyCapacity_},{"history_retained",history_.size()},
            {"selected_capacity",selectedCapacity_},{"selected",selected_.size()},{"selected_retained",selected_.size()},
            {"put_requests",putRequests_},{"created",created_},{"duplicate_puts",duplicatePuts_},
            {"evictions",historyEvictions_},{"history_evictions",historyEvictions_},{"history_capacity_drops",historyCapacityDrops_},
            {"selection_requests",selectionRequests_},{"selected_total",selectedTotal_},{"duplicate_selections",duplicateSelections_},
            {"selected_evictions",selectedEvictions_},{"selected_capacity_drops",selectedCapacityDrops_},{"missing",missing_},
            {"selection_revision",selectionRevision_},{"selection_revision_exhausted",revisionExhausted_},
            {"selection_revision_drops",revisionDrops_},{"counter_overflow",counterOverflow_},
            {"duplicate_check_scope","retained_history_and_selected"}};
    }

    Json Export() const
    {
        Json result=Stats();
        result["records"]=Json::object();
        for (const auto &record:selected_) result["records"][record.first]=record.second;
        return result;
    }
};
}
