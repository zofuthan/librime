//
// Copyleft RIME Developers
// License: GPLv3
//
// 2013-05-26 GONG Chen <chen.sst@gmail.com>
//

#include <ctime>
#include <rime/candidate.h>
#include <rime/common.h>
#include <rime/config.h>
#include <rime/schema.h>
#include <rime/switcher.h>
#include <rime/translation.h>
#include <rime/gear/schema_list_translator.h>

namespace rime {

class SchemaSelection : public SimpleCandidate, public SwitcherCommand {
 public:
  SchemaSelection(Schema* schema)
      : SimpleCandidate("schema", 0, 0, schema->schema_name()),
        SwitcherCommand(schema->schema_id()) {
  }

  virtual void Apply(Switcher* switcher);
};

void SchemaSelection::Apply(Switcher* switcher) {
  Engine* engine = switcher->attached_engine();
  if (!engine)
    return;
  if (keyword_ != engine->schema()->schema_id()) {
    switcher->ApplySchema(new Schema(keyword_));
  }
  if (Config* user_config = switcher->user_config()) {
    user_config->SetString("var/previously_selected_schema", keyword_);
    user_config->SetInt("var/schema_access_time/" + keyword_, time(NULL));
  }
}

class SchemaListTranslation : public FifoTranslation {
 public:
  SchemaListTranslation(Switcher* switcher) {
    LoadSchemaList(switcher);
  }
  virtual int Compare(shared_ptr<Translation> other,
                      const CandidateList& candidates);

 protected:
  void LoadSchemaList(Switcher* switcher);
};

int SchemaListTranslation::Compare(shared_ptr<Translation> other,
                                   const CandidateList& candidates) {
  if (!other || other->exhausted())
    return -1;
  if (exhausted())
    return 1;
  // switches should immediately follow current schema (#0)
  auto theirs = other->Peek();
  if (theirs && theirs->type() == "switch") {
    return cursor_ == 0 ? -1 : 1;
  }
  return Translation::Compare(other, candidates);
}

void SchemaListTranslation::LoadSchemaList(Switcher* switcher) {
  Engine* engine = switcher->attached_engine();
  if (!engine)
    return;
  Config* config = switcher->schema()->config();
  if (!config)
    return;
  auto schema_list = config->GetList("schema_list");
  if (!schema_list)
    return;
  // current schema comes first
  Schema* current_schema = engine->schema();
  if (current_schema) {
    Append(New<SchemaSelection>(current_schema));
  }
  Config* user_config = switcher->user_config();
  size_t fixed = candies_.size();
  time_t now = time(NULL);
  // load the rest schema list
  for (size_t i = 0; i < schema_list->size(); ++i) {
    auto item = As<ConfigMap>(schema_list->GetAt(i));
    if (!item)
      continue;
    auto schema_property = item->GetValue("schema");
    if (!schema_property)
      continue;
    const std::string& schema_id(schema_property->str());
    if (current_schema && schema_id == current_schema->schema_id())
      continue;
    Schema schema(schema_id);
    auto cand = New<SchemaSelection>(&schema);
    int timestamp = 0;
    if (user_config &&
        user_config->GetInt("var/schema_access_time/" + schema_id,
                            &timestamp)) {
      if (timestamp <= now)
        cand->set_quality(timestamp);
    }
    Append(cand);
  }
  DLOG(INFO) << "num schemata: " << candies_.size();
  bool fix_order = false;
  config->GetBool("switcher/fix_schema_list_order", &fix_order);
  if (fix_order)
    return;
  // reorder schema list by recency
  std::stable_sort(candies_.begin() + fixed, candies_.end(),
      [](const shared_ptr<Candidate>& a, const shared_ptr<Candidate>& b) {
        return a->quality() > b->quality();
      });
}

SchemaListTranslator::SchemaListTranslator(const Ticket& ticket)
    : Translator(ticket) {
}

shared_ptr<Translation> SchemaListTranslator::Query(const std::string& input,
                                                    const Segment& segment,
                                                    std::string* prompt) {
  auto switcher = dynamic_cast<Switcher*>(engine_);
  if (!switcher) {
    return nullptr;
  }
  return New<SchemaListTranslation>(switcher);
}

}  // namespace rime
