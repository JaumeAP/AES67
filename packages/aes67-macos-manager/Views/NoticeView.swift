//
// NoticeView.swift
// AES67 Manager
// The placeholder both apps show where a list would be when the list is
// empty, or when something stopped it from being filled.
//
// Written out identically as a private `notice(_:icon:)` in the Manager's
// DiscoveredSessionsView and in the Controller's ControllerWindow. The same
// screen in two applications has to look the same, and two copies of the
// spacing, the sizes and the colours is not how that stays true.
//
// It lives here, with the Manager's views, because the Controller's build.sh
// already compiles four of them -- the sharing is established, and adding a
// fifth is cheaper than inventing a module for one view.
//

import SwiftUI

struct NoticeView: View {
    /// What happened, in a sentence the user can act on.
    let text: String
    /// An SF Symbol name.
    let icon: String

    var body: some View {
        VStack(spacing: 10) {
            Image(systemName: icon)
                .font(.system(size: 32))
                .foregroundColor(.secondary)
            Text(text)
                .font(.callout)
                .foregroundColor(.secondary)
                .multilineTextAlignment(.center)
                .fixedSize(horizontal: false, vertical: true)
        }
        .padding(40)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}
