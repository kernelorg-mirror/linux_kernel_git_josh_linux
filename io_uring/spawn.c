// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Spawning a linked series of operations onto a dedicated task.
 *
 * Copyright © 2022 Josh Triplett
 */

#include <linux/binfmts.h>
#include <linux/nospec.h>
#include <linux/syscalls.h>

#include "io_uring.h"
#include "rsrc.h"
#include "spawn.h"

/* FIXME: Put this in a header */
int io_issue_sqe(struct io_kiocb *req, unsigned int issue_flags);

struct io_exec {
	struct file *file_unused;
	const char __user *filename;
	const char __user *const __user *argv;
	const char __user *const __user *envp;
};

struct io_clone {
	struct file *file_unused;
	struct io_kiocb *link;
};

static int io_uring_spawn_task(void *data)
{
	struct io_kiocb *head = data;
	struct io_clone *c = io_kiocb_to_cmd(head, struct io_clone);
	struct io_ring_ctx *ctx = head->ctx;
	struct io_submit_state *state = &ctx->submit_state;
	struct io_kiocb *req;
	int ret;

	mutex_lock(&ctx->uring_lock);

	while ((req = c->link) != NULL) {
		bool is_hardlink = req->flags & REQ_F_HARDLINK;

		c->link = req->link;
		req->link = NULL;
		req->flags &= ~(REQ_F_HARDLINK | REQ_F_LINK);

		ret = io_issue_sqe(req, IO_URING_F_COMPLETE_DEFER);
		if (ret) {
			wq_list_add_tail(&req->comp_list, &state->compl_reqs);
			if (!is_hardlink)
				break;
		} else if (req->opcode == IORING_OP_EXEC) {
			/* Don't allow further operations after a successful
			 * exec, and don't kill the process. */
			io_submit_flush_completions(ctx);
			mutex_unlock(&ctx->uring_lock);
			return 0;
		}
	}

	wq_list_add_tail(&head->comp_list, &state->compl_reqs);
	io_submit_flush_completions(ctx);
	mutex_unlock(&ctx->uring_lock);
	force_exit_sig(SIGKILL);
	return 0;
}

/* FIXME: Put this in a header */
struct task_struct *create_io_uring_spawn_task(int (*fn)(void *), void *arg);

int io_clone_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe)
{
	struct io_clone *c = io_kiocb_to_cmd(req, struct io_clone);

	c->link = NULL;
	return 0;
}

int io_clone(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_clone *c = io_kiocb_to_cmd(req, struct io_clone);
	struct task_struct *tsk;

	c->link = req->link;
	req->flags &= ~(REQ_F_HARDLINK | REQ_F_LINK);
	req->link = NULL;

	/* FIXME: remember to fail links here */
	tsk = create_io_uring_spawn_task(io_uring_spawn_task, req);
	if (IS_ERR(tsk))
		return PTR_ERR(tsk);
	wake_up_new_task(tsk);
	return IOU_OK;
}

int io_exec_prep(struct io_kiocb *req, const struct io_uring_sqe *sqe)
{
	struct io_exec *e = io_kiocb_to_cmd(req, typeof(*e));

	if (unlikely(sqe->fd || sqe->buf_index || sqe->len || sqe->rw_flags || sqe->file_index))
		return -EINVAL;

	e->filename = u64_to_user_ptr(READ_ONCE(sqe->addr));
	e->argv = u64_to_user_ptr(READ_ONCE(sqe->addr2));
	e->envp = u64_to_user_ptr(READ_ONCE(sqe->addr3));

	return 0;
}

/* FIXME: should be in a header */
int do_execve(struct filename *filename,
	const char __user *const __user *__argv,
	const char __user *const __user *__envp);

int io_exec(struct io_kiocb *req, unsigned int issue_flags)
{
	struct io_exec *e = io_kiocb_to_cmd(req, typeof(*e));
	int ret;

	ret = do_execve(getname(e->filename), e->argv, e->envp);
	if (ret)
		return ret;
	return IOU_OK;
}
