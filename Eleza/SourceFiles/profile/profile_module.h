// The profile module: profiles (`user/<id>`, `u/<handle>`), the «من» tab,
// follow lists, follow requests and «ویرایش نمایه».
//
// Besides the canonical routes it answers a few of its own (used by links
// inside the module and by --debug-route):
//
//   me/profile            my profile as a main section (the single column layout)
//   me/bookmarks          … on the «ذخیره‌ها» tab
//   me/edit               «ویرایش نمایه»
//   me/requests           «درخواست‌های دنبال کردن»
//   u/<handle>/followers  u/<handle>/following
//   user/<id>/followers   user/<id>/following
//   debug/birthday        the birthday picker;  debug/share  the share box
#pragma once

namespace Profile {

void Register();

} // namespace Profile
