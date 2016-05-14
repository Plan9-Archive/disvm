//
// Dis VM
// File: tool_dispatch.cpp
// Author: arr
//

#include <cassert>
#include <debug.h>
#include <vm_tools.h>
#include <exceptions.h>
#include <unordered_set>
#include <utils.h>
#include "tool_dispatch.h"

using namespace disvm;
using namespace disvm::runtime;

// Empty destructor for vm tool 'interface'
vm_tool_t::~vm_tool_t()
{
}

vm_tool_dispatch_t::vm_tool_dispatch_t(vm_t &vm)
    : _vm{ vm }
{
    _events.cookie_counter = 1;
    _breakpoints.cookie_counter = 1;
}

vm_tool_dispatch_t::~vm_tool_dispatch_t()
{
    std::lock_guard<std::mutex> lock{ _tools_lock };
    for (auto &t : _tools)
        t.second->on_unload();
}

std::size_t vm_tool_dispatch_t::load_tool(std::shared_ptr<runtime::vm_tool_t> tool)
{
    static std::size_t _tool_id_counter{ 1 };

    if (tool == nullptr)
        throw vm_system_exception{ "Tool cannot be null" };

    // Keep the lock for the collection until the tool has been completed being loaded.
    std::lock_guard<std::mutex> lock{ _tools_lock };
    const auto current_id = _tool_id_counter++;
    _tools[current_id] = tool;

    try
    {
        // [TODO] For debug builds, it might be useful to wrap the controller in a proxy that records
        // the subscribed callbacks and asserts they are all properly unsubscribed during agent unload.
        tool->on_load(_vm, *this, current_id);
    }
    catch (...)
    {
        if (debug::is_component_tracing_enabled<debug::component_trace_t::tool>())
            debug::log_msg(debug::component_trace_t::tool, debug::log_level_t::warning, "load: tool: failure");

        // The ID is tainted and will not be re-used
        _tools.erase(current_id);
        throw;
    }

    if (debug::is_component_tracing_enabled<debug::component_trace_t::tool>())
        debug::log_msg(debug::component_trace_t::tool, debug::log_level_t::debug, "load: tool: %d", current_id);

    return current_id;
}

std::size_t vm_tool_dispatch_t::unload_tool(std::size_t tool_id)
{
    // Keep the lock for the collection until the tool has been completed being unloaded.
    std::lock_guard<std::mutex> lock{ _tools_lock };
    auto iter = _tools.find(tool_id);
    if (iter == _tools.cend())
        return _tools.size();

    auto tool = iter->second;
    _tools.erase(iter);

    if (debug::is_component_tracing_enabled<debug::component_trace_t::tool>())
        debug::log_msg(debug::component_trace_t::tool, debug::log_level_t::debug, "unload: tool: %d", tool_id);

    assert(tool != nullptr);
    tool->on_unload();

    return _tools.size();
}

opcode_t vm_tool_dispatch_t::on_breakpoint(vm_registers_t &r, vm_t &vm)
{
    return get_original_opcode(r.module_ref->module.get(), r.pc - 1);
}

std::size_t vm_tool_dispatch_t::subscribe_event(vm_event_t evt, vm_event_callback_t cb)
{
    if (!cb)
        throw vm_system_exception{ "Invalid callback for event" };

    std::lock_guard<std::mutex> lock{ _events.lock };

    const auto cookie_id = _events.cookie_counter++;

    // Store the event callback
    auto &event_callbacks = _events.callbacks[evt];
    event_callbacks[cookie_id] = cb;

    // Store the cookie with the associate event
    _events.cookie_to_event[cookie_id] = evt;

    if (debug::is_component_tracing_enabled<debug::component_trace_t::tool>())
        debug::log_msg(debug::component_trace_t::tool, debug::log_level_t::debug, "subscribe: event: %d %d", evt, cookie_id);

    return cookie_id;
}

void vm_tool_dispatch_t::unsubscribe_event(std::size_t cookie_id)
{
    std::lock_guard<std::mutex> lock{ _events.lock };

    auto iter = _events.cookie_to_event.find(cookie_id);
    if (iter == _events.cookie_to_event.cend())
        return;

    // Erase cookie and event
    const auto evt = iter->second;
    auto &event_callbacks = _events.callbacks[evt];
    event_callbacks.erase(cookie_id);
    _events.cookie_to_event.erase(iter);

    if (debug::is_component_tracing_enabled<debug::component_trace_t::tool>())
        debug::log_msg(debug::component_trace_t::tool, debug::log_level_t::debug, "unsubscribe: event: %d %d", evt, cookie_id);
}

std::size_t vm_tool_dispatch_t::set_breakpoint(std::shared_ptr<const vm_module_t> module, vm_pc_t pc)
{
    if (module == nullptr || util::has_flag(module->header.runtime_flag, runtime_flags_t::builtin))
        throw vm_system_exception{ "Unable to set breakpoint in supplied module" };

    // [TODO] This should be re-considered. Removing 'const' is less than ideal.
    // Remove the const value since the code section needs to be updated.
    auto &code_section = const_cast<code_section_t &>(module->code_section);
    if (pc >= code_section.size())
        throw vm_system_exception{ "Invalid PC for module" };

    const auto real_opcode = code_section[pc].op.opcode;

    {
        std::lock_guard<std::mutex> lock{ _breakpoints.lock };
        if (real_opcode == opcode_t::brkpt)
            throw vm_system_exception{ "Breakpoint already set at PC" };

        const auto cookie_id = _breakpoints.cookie_counter++;

        // Map the cookie to the module/pc pair.
        _breakpoints.cookie_to_modulepc[cookie_id] = std::make_pair(module, pc);

        // Record the original opcode
        auto &pc_map = _breakpoints.original_opcodes[reinterpret_cast<std::uintptr_t>(module.get())];
        pc_map[pc] = real_opcode;

        // Replace the current opcode with breakpoint
        code_section[pc].op.opcode = opcode_t::brkpt;

        if (debug::is_component_tracing_enabled<debug::component_trace_t::tool>())
            debug::log_msg(debug::component_trace_t::tool, debug::log_level_t::debug, "breakpoint: set: %d %d >>%s<<", cookie_id, pc, module->module_name->str());

        return cookie_id;
    }
}

void vm_tool_dispatch_t::clear_breakpoint(std::size_t cookie_id)
{
    std::lock_guard<std::mutex> lock{ _breakpoints.lock };

    // Resolve the cookie to the module/pc pair
    auto iter_cookie = _breakpoints.cookie_to_modulepc.find(cookie_id);
    if (iter_cookie == _breakpoints.cookie_to_modulepc.cend())
        return;

    auto mod_pc_pair = iter_cookie->second;
    const auto target_pc = mod_pc_pair.second;

    // Determine the original opcode
    auto iter_orig = _breakpoints.original_opcodes.find(reinterpret_cast<std::uintptr_t>(mod_pc_pair.first.get()));
    assert(iter_orig != _breakpoints.original_opcodes.cend());

    auto &pc_map = iter_orig->second;
    auto iter_pc = pc_map.find(target_pc);
    assert(iter_pc != pc_map.cend());
    
    auto original_opcode = iter_pc->second;

    // Replace the breakpoint opcode with the original
    auto &code_section = const_cast<code_section_t &>(mod_pc_pair.first->code_section);
    assert(code_section[target_pc].op.opcode == opcode_t::brkpt);
    code_section[target_pc].op.opcode = original_opcode;

    if (debug::is_component_tracing_enabled<debug::component_trace_t::tool>())
        debug::log_msg(debug::component_trace_t::tool, debug::log_level_t::debug, "breakpoint: unset: %d %d >>%s<<", cookie_id, target_pc, mod_pc_pair.first->module_name->str());

    // Clean-up the PC mapping to opcode
    pc_map.erase(iter_pc);

    // If the PC map is empty, erase the module mapping as well
    if (pc_map.size() == 0)
        _breakpoints.original_opcodes.erase(iter_orig);

    // Remove the cookie map
    _breakpoints.cookie_to_modulepc.erase(iter_cookie);
}

opcode_t vm_tool_dispatch_t::get_original_opcode(const vm_module_t *module, vm_pc_t pc)
{
    if (module == nullptr || util::has_flag(module->header.runtime_flag, runtime_flags_t::builtin))
        throw vm_system_exception{ "Unable to determine original opcode in supplied module" };

    std::lock_guard<std::mutex> lock{ _breakpoints.lock };

    auto iter_orig = _breakpoints.original_opcodes.find(reinterpret_cast<std::uintptr_t>(module));
    if (iter_orig != _breakpoints.original_opcodes.cend())
    {
        auto &pc_map = iter_orig->second;
        auto iter_pc = pc_map.find(pc);
        if (iter_pc != pc_map.cend())
            return iter_pc->second;
    }

    assert(false && "Why are we missing this information?");
    return opcode_t::runt; // Return NOP instruction?
}