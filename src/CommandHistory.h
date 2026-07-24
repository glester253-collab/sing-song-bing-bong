#pragma once
// CommandHistory.h — lightweight undo/redo for clip-editing operations.
//
// MESSAGE THREAD ONLY.  All public methods must be called from the message
// (UI) thread.  The audio thread never touches CommandHistory.
//
// Design:
//   - Each undoable action is represented by an ICommand with do/undo methods.
//   - CommandHistory owns a linear history deque with a cursor.
//   - Calling execute() runs the command and pushes it onto the redo-cleared
//     future stack.
//   - undo() and redo() step the cursor and re-apply commands.
//   - maxDepth controls memory use; oldest entries are dropped when exceeded.
//
// Clients implement ICommand for their specific action types (e.g. ClipMoveCommand,
// ClipTrimCommand).  Each command captures the full before/after state so that
// undo is always safe regardless of intervening operations.

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ssbb {

// ---- ICommand interface --------------------------------------------------

struct ICommand
{
    virtual ~ICommand() = default;

    /// Human-readable name shown in menus (e.g. "Move Clip", "Trim Start").
    [[nodiscard]]
    virtual std::string name() const = 0;

    /// Apply / re-apply the command.
    virtual void execute() = 0;

    /// Reverse the command.
    virtual void undo() = 0;
};

// ---- Concrete command: lambda pair (for simple stateless operations) ------

class LambdaCommand final : public ICommand
{
public:
    LambdaCommand(std::string          name,
                  std::function<void()> doFn,
                  std::function<void()> undoFn)
        : name_(std::move(name))
        , doFn_(std::move(doFn))
        , undoFn_(std::move(undoFn))
    {}

    [[nodiscard]] std::string name() const override { return name_; }
    void execute() override { doFn_(); }
    void undo()    override { undoFn_(); }

private:
    std::string           name_;
    std::function<void()> doFn_;
    std::function<void()> undoFn_;
};

// ---- CommandHistory ------------------------------------------------------

class CommandHistory
{
public:
    explicit CommandHistory(std::size_t maxDepth = 100)
        : maxDepth_(maxDepth)
    {}

    // Non-copyable
    CommandHistory(const CommandHistory&)            = delete;
    CommandHistory& operator=(const CommandHistory&) = delete;

    /// Execute `cmd`, push it onto the history, and clear any redo stack.
    void execute(std::unique_ptr<ICommand> cmd)
    {
        if (!cmd) return;

        cmd->execute();

        // Drop redo stack.
        if (cursor_ < history_.size())
            history_.erase(history_.begin() + static_cast<ptrdiff_t>(cursor_),
                           history_.end());

        history_.push_back(std::move(cmd));
        cursor_ = history_.size();

        // Trim oldest if over limit.
        if (history_.size() > maxDepth_)
        {
            const std::size_t drop = history_.size() - maxDepth_;
            history_.erase(history_.begin(),
                           history_.begin() + static_cast<ptrdiff_t>(drop));
            cursor_ = history_.size();
        }
    }

    /// Convenience: build a LambdaCommand and execute it.
    void execute(std::string           name,
                 std::function<void()> doFn,
                 std::function<void()> undoFn)
    {
        execute(std::make_unique<LambdaCommand>(
            std::move(name), std::move(doFn), std::move(undoFn)));
    }

    /// Undo the most recent command.  Returns false if nothing to undo.
    bool undo()
    {
        if (cursor_ == 0) return false;
        --cursor_;
        history_[cursor_]->undo();
        return true;
    }

    /// Redo the next command.  Returns false if nothing to redo.
    bool redo()
    {
        if (cursor_ >= history_.size()) return false;
        history_[cursor_]->execute();
        ++cursor_;
        return true;
    }

    /// True if undo() would succeed.
    [[nodiscard]] bool canUndo() const noexcept { return cursor_ > 0; }

    /// True if redo() would succeed.
    [[nodiscard]] bool canRedo() const noexcept { return cursor_ < history_.size(); }

    /// Name of the command that would be undone next.
    [[nodiscard]] std::string undoName() const
    {
        return canUndo() ? history_[cursor_ - 1]->name() : "";
    }

    /// Name of the command that would be redone next.
    [[nodiscard]] std::string redoName() const
    {
        return canRedo() ? history_[cursor_]->name() : "";
    }

    /// Discard all history.
    void clear()
    {
        history_.clear();
        cursor_ = 0;
    }

    [[nodiscard]] std::size_t size()   const noexcept { return history_.size(); }
    [[nodiscard]] std::size_t cursor() const noexcept { return cursor_; }

private:
    std::vector<std::unique_ptr<ICommand>> history_;
    std::size_t                            cursor_   { 0 };
    std::size_t                            maxDepth_;
};

} // namespace ssbb
