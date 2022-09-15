// Released under the MIT License. See LICENSE for details.

#include "ballistica/ui/widget/widget.h"

#include "ballistica/logic/logic.h"
#include "ballistica/python/class/python_class_widget.h"
#include "ballistica/python/python_context_call.h"
#include "ballistica/ui/ui.h"
#include "ballistica/ui/widget/container_widget.h"
#include "ballistica/ui/widget/root_widget.h"

namespace ballistica {

Widget::Widget() = default;

Widget::~Widget() {
  // Release our ref to ourself if we have one.
  if (py_ref_) {
    Py_DECREF(py_ref_);
  }

  // The very last thing we do is call our on-delete calls.
  // We need to be prepared for anything happening as a result of this,
  // so let's work off a copy of our callback list in case it gets mucked with.
  auto on_delete_calls = on_delete_calls_;
  for (auto&& i : on_delete_calls) {
    i->Run();
  }
}

void Widget::SetToolbarVisibility(ToolbarVisibility v) {
  toolbar_visibility_ = v;
  // Most widgets can never influence the global toolbar so we can
  // do a quick out.
  if (parent_widget_ != nullptr && parent_widget_->is_window_stack()) {
    g_ui->root_widget()->UpdateForFocusedWindow();
  }
}

void Widget::SetDepthRange(float min_depth, float max_depth) {
  BA_PRECONDITION(min_depth >= 0.0f && min_depth <= 1.0f);
  BA_PRECONDITION(max_depth >= min_depth && max_depth <= 1.0f);
  depth_range_min_ = min_depth;
  depth_range_max_ = max_depth;
}

auto Widget::IsInMainStack() const -> bool {
  if (!g_ui) {
    BA_LOG_ONCE(LogLevel::kError,
                "Widget::IsInMainStack() called before ui creation.");
    return false;
  }
  // Navigate up to the top of the hierarchy and see if the
  // screen-root widget is in there somewhere.
  ContainerWidget* screen_root = g_ui->screen_root_widget();
  assert(screen_root);
  if (!screen_root) {
    return false;
  }
  ContainerWidget* parent = parent_widget_;
  while (parent != nullptr) {
    if (parent == screen_root) {
      return true;
    }
    parent = parent->parent_widget_;
  }
  return false;
}

auto Widget::IsInOverlayStack() const -> bool {
  // Navigate up to the top of the hierarchy and see if the overlay-root widget
  // is in there somewhere.
  ContainerWidget* overlay_root = g_ui->overlay_root_widget();
  assert(overlay_root);
  ContainerWidget* parent = parent_widget_;
  while (parent != nullptr) {
    if (parent == overlay_root) {
      return true;
    }
    parent = parent->parent_widget_;
  }
  return false;
}

void Widget::SetSelected(bool s, SelectionCause cause) {
  if (selected_ == s) return;
  selected_ = s;
  if (selected_ && on_select_call_.exists()) {
    // Call this in the next cycle (don't wanna risk mucking
    // with UI from within a UI loop).
    g_logic->PushPythonWeakCall(
        Object::WeakRef<PythonContextCall>(on_select_call_));
  }
}

auto Widget::IsHierarchySelected() const -> bool {
  const Widget* p = this;
  while (true) {
    if (!p->selected()) {
      return false;
    }
    p = p->GetOwnerWidget();
    if (!p || p == g_ui->root_widget()) {
      break;
    }
  }
  return true;
}

void Widget::SetOnSelectCall(PyObject* call_obj) {
  on_select_call_ = Object::New<PythonContextCall>(call_obj);
}

void Widget::AddOnDeleteCall(PyObject* call_obj) {
  on_delete_calls_.push_back(Object::New<PythonContextCall>(call_obj));
}

void Widget::GlobalSelect() {
  Widget* w = this;
  ContainerWidget* c = parent_widget();
  if (!c) {
    return;
  }
  while (true) {
    c->SelectWidget(w);
    w = c;
    c = c->parent_widget();
    if (!c) {
      break;
    }
  }
}

void Widget::Show() {
  Widget* w = this;
  ContainerWidget* c = parent_widget();
  if (!c) {
    return;
  }
  while (true) {
    c->ShowWidget(w);
    w = c;
    c = c->parent_widget();
    if (!c) {
      break;
    }
  }
}

auto Widget::GetOwnerWidget() const -> Widget* {
  return parent_widget_ ? parent_widget_ : owner_widget_;
}

void Widget::WidgetPointToScreen(float* x, float* y) const {
  const ContainerWidget* w = parent_widget();

  // If we have no parent, we're the root widget and we're already in our own
  // space.
  if (w) {
    std::vector<const ContainerWidget*> widgets;
    while (w) {
      widgets.push_back(w);
      w = w->parent_widget();
    }
    widgets[0]->TransformPointFromChild(x, y, *this);
    for (size_t i = 1; i < widgets.size(); i++) {
      widgets[i]->TransformPointFromChild(x, y, *widgets[i - 1]);
    }
  }
}

auto Widget::GetDrawBrightness(millisecs_t current_time) const -> float {
  return 1.0f;
}

void Widget::ScreenPointToWidget(float* x, float* y) const {
#if BA_DEBUG_BUILD || BA_TEST_BUILD
  float x_old = *x;
  float y_old = *y;
#endif

  const ContainerWidget* w = parent_widget();

  // If we have no parent, we're the root widget and we're
  // already in our own space.
  if (w) {
    std::vector<const ContainerWidget*> widgets;
    while (w) {
      widgets.push_back(w);
      w = w->parent_widget();
    }
    for (int i = static_cast<int>(widgets.size() - 1); i > 0; i--) {
      widgets[i]->TransformPointToChild(x, y, *widgets[i - 1]);
    }
    widgets[0]->TransformPointToChild(x, y, *this);
  }

  // Sanity test: do the reverse and make sure it comes out the same.
#if BA_DEBUG_BUILD || BA_TEST_BUILD
  float x_test = *x;
  float y_test = *y;
  WidgetPointToScreen(&x_test, &y_test);
  if (std::abs(x_test - x_old) > 0.01f || std::abs(y_test - y_old) > 0.01f) {
    Log(LogLevel::kError,
        "ScreenPointToWidget sanity check error: expected ("
            + std::to_string(x_old) + "," + std::to_string(y_old) + ") got ("
            + std::to_string(x_test) + "," + std::to_string(y_test) + ")");
  }
#endif  // BA_DEBUG_BUILD || BA_TEST_BUILD
}

auto Widget::GetPyWidget(bool new_ref) -> PyObject* {
  assert(InLogicThread());
  if (py_ref_ == nullptr) {
    py_ref_ = PythonClassWidget::Create(this);
  }
  if (new_ref) {
    Py_INCREF(py_ref_);
  }
  return py_ref_;
}

void Widget::GetCenter(float* x, float* y) {
  *x = tx() + scale() * GetWidth() * 0.5f;
  *y = ty() + scale() * GetHeight() * 0.5f;
}

auto Widget::HandleMessage(const WidgetMessage& m) -> bool { return false; }

void Widget::Draw(RenderPass* pass, bool transparent) {}

auto Widget::IsSelectable() -> bool { return false; }

auto Widget::IsSelectableViaKeys() -> bool { return true; }

auto Widget::IsAcceptingInput() const -> bool { return true; }

void Widget::Activate() {}

}  // namespace ballistica
