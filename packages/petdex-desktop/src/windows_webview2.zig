const std = @import("std");

const Host = opaque {};

extern fn petdex_webview2_create(asset_root: [*:0]const u16, config_dir: [*:0]const u16, width: c_int, height: c_int) ?*Host;
extern fn petdex_webview2_destroy(host: *Host) void;
extern fn petdex_webview2_run(host: *Host) c_int;

const WINDOW_W: c_int = 140;
const WINDOW_H: c_int = 180;

pub fn run(allocator: std.mem.Allocator, asset_root: []const u8, config_dir: []const u8) !void {
    const asset_root_w = try std.unicode.utf8ToUtf16LeAllocZ(allocator, asset_root);
    defer allocator.free(asset_root_w);
    const config_dir_w = try std.unicode.utf8ToUtf16LeAllocZ(allocator, config_dir);
    defer allocator.free(config_dir_w);

    const host = petdex_webview2_create(asset_root_w.ptr, config_dir_w.ptr, WINDOW_W, WINDOW_H) orelse return error.WebView2CreateFailed;
    defer petdex_webview2_destroy(host);

    const code = petdex_webview2_run(host);
    if (code != 0) return error.WebView2RunFailed;
}
