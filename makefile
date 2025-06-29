.PHONY: dev

dev:
	exec tmux new-session -d -s dev_session 'cd ./server; bash' \;\
	split-window -h 'cd ./client; bash' \;\
	select-pane -t 0 \;\
	attach-session -t dev_session
