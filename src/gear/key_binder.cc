//
// Copyleft RIME Developers
// License: GPLv3
//
// 2011-11-23 GONG Chen <chen.sst@gmail.com>
//
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <rime/common.h>
#include <rime/composition.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/key_event.h>
#include <rime/key_table.h>
#include <rime/schema.h>
#include <rime/switcher.h>
#include <rime/gear/key_binder.h>

using namespace std::placeholders;

namespace rime {

enum KeyBindingCondition {
  kNever,
  kWhenPaging,     // user has changed page
  kWhenHasMenu,    // at least one candidate
  kWhenComposing,  // input string is not empty
  kAlways,
};

static struct KeyBindingConditionDef {
  KeyBindingCondition condition;
  const char* name;
} condition_definitions[] = {
  { kWhenPaging,    "paging"    },
  { kWhenHasMenu,   "has_menu"  },
  { kWhenComposing, "composing" },
  { kAlways,        "always"    },
  { kNever,         NULL        }
};

static KeyBindingCondition translate_condition(const std::string& str) {
  for (auto* d = condition_definitions; d->name; ++d) {
    if (str == d->name)
      return d->condition;
  }
  return kNever;
}

struct KeyBinding {
  KeyBindingCondition whence;
  KeyEvent target;
  std::function<void (Engine* engine)> action;

  bool operator< (const KeyBinding& o) const {
    return whence < o.whence;
  }
};

class KeyBindings : public std::map<KeyEvent,
                                    std::vector<KeyBinding>> {
 public:
  void LoadBindings(const ConfigListPtr& bindings);
  void Bind(const KeyEvent& key, const KeyBinding& binding);
};

static void toggle_option(Engine* engine, const std::string& option) {
  if (!engine)
    return;
  Context* ctx = engine->context();
  ctx->set_option(option, !ctx->get_option(option));
}

static void select_schema(Engine* engine, const std::string& schema) {
  if (!engine)
    return;
  auto* switcher = dynamic_cast<Switcher*>(engine->attached_engine());
  if (!switcher)
    return;
  if (schema == ".next") {
    switcher->SelectNextSchema();
  }
  else {
    switcher->ApplySchema(new Schema(schema));
  }
}

void KeyBindings::LoadBindings(const ConfigListPtr& bindings) {
  if (!bindings)
    return;
  for (size_t i = 0; i < bindings->size(); ++i) {
    auto map = As<ConfigMap>(bindings->GetAt(i));
    if (!map)
      continue;
    auto whence = map->GetValue("when");
    if (!whence)
      continue;
    auto pattern = map->GetValue("accept");
    if (!pattern)
      continue;
    KeyBinding binding;
    binding.whence = translate_condition(whence->str());
    if (binding.whence == kNever) {
      continue;
    }
    KeyEvent key;
    if (!key.Parse(pattern->str())) {
      LOG(WARNING) << "invalid key binding #" << i << ".";
      continue;
    }
    if (auto target = map->GetValue("send")) {
      if (!binding.target.Parse(target->str())) {
        LOG(WARNING) << "invalid key binding #" << i << ".";
        continue;
      }
    }
    else if (auto option = map->GetValue("toggle")) {
      binding.action = std::bind(&toggle_option, _1, option->str());
    }
    else if (auto schema = map->GetValue("select")) {
      binding.action = std::bind(&select_schema, _1, schema->str());
    }
    else {
      LOG(WARNING) << "invalid key binding #" << i << ".";
      continue;
    }
    Bind(key, binding);
  }
}

void KeyBindings::Bind(const KeyEvent& key, const KeyBinding& binding) {
  auto& vec = (*this)[key];
  // insert before existing binding of the same condition
  auto lb = std::lower_bound(vec.begin(), vec.end(), binding);
  vec.insert(lb, binding);
}

KeyBinder::KeyBinder(const Ticket& ticket) : Processor(ticket),
                                             key_bindings_(new KeyBindings),
                                             redirecting_(false),
                                             last_key_(0) {
  LoadConfig();
}

class KeyBindingConditions : public std::set<KeyBindingCondition> {
 public:
  explicit KeyBindingConditions(Context* ctx);
};

KeyBindingConditions::KeyBindingConditions(Context* ctx) {
  insert(kAlways);

  if (ctx->IsComposing()) {
    insert(kWhenComposing);
  }

  if (ctx->HasMenu() && !ctx->get_option("ascii_mode")) {
    insert(kWhenHasMenu);
  }

  Composition* comp = ctx->composition();
  if (!comp->empty() && comp->back().HasTag("paging")) {
    insert(kWhenPaging);
  }
}

ProcessResult KeyBinder::ProcessKeyEvent(const KeyEvent& key_event) {
  if (redirecting_ || !key_bindings_ || key_bindings_->empty())
    return kNoop;
  if (ReinterpretPagingKey(key_event))
    return kNoop;
  if (key_bindings_->find(key_event) == key_bindings_->end())
    return kNoop;
  KeyBindingConditions conditions(engine_->context());
  for (const KeyBinding& binding : (*key_bindings_)[key_event]) {
    if (conditions.find(binding.whence) == conditions.end())
      continue;
    PerformKeyBinding(binding);
    return kAccepted;
  }
  // not handled
  return kNoop;
}

void KeyBinder::PerformKeyBinding(const KeyBinding& binding) {
  if (binding.action) {
    binding.action(engine_);
  }
  else {
    redirecting_ = true;
    engine_->ProcessKeyEvent(binding.target);
    redirecting_ = false;
  }
}

void KeyBinder::LoadConfig() {
  if (!engine_)
    return;
  Config* config = engine_->schema()->config();
  std::string preset;
  if (config->GetString("key_binder/import_preset", &preset)) {
    unique_ptr<Config> preset_config(Config::Require("config")->Create(preset));
    if (!preset_config) {
      LOG(ERROR) << "Error importing preset key bindings '" << preset << "'.";
      return;
    }
    if (auto bindings = preset_config->GetList("key_binder/bindings"))
      key_bindings_->LoadBindings(bindings);
    else
      LOG(WARNING) << "missing preset key bindings.";
  }
  // per schema configuration, overriding preset bindings
  if (auto bindings = config->GetList("key_binder/bindings"))
    key_bindings_->LoadBindings(bindings);
}

bool KeyBinder::ReinterpretPagingKey(const KeyEvent& key_event) {
  if (key_event.release())
    return false;
  bool ret = false;
  int ch = (key_event.modifier() == 0) ? key_event.keycode() : 0;
  // reinterpret period key followed by alphabetic keys
  // unless period/comma key has been used multiple times
  if (ch == '.' && (last_key_ == '.' || last_key_ == ',')) {
    last_key_ = 0;
    return ret;
  }
  if (last_key_ == '.' && ch >= 'a' && ch <= 'z') {
    Context* ctx = engine_->context();
    const std::string& input(ctx->input());
    if (!input.empty() && input[input.length() - 1] != '.') {
      LOG(INFO) << "reinterpreted key: '" << last_key_
                << "', successor: '" << (char)ch << "'";
      ctx->PushInput(last_key_);
      ret = true;
    }
  }
  last_key_ = ch;
  return ret;
}

}  // namespace rime
