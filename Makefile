CC = gcc
CFLAGS = -Wall -Wextra -Iinclude -g -O0
BUILD_DIR = build
SRC_DIR = src
OBJ_DIR = $(BUILD_DIR)/obj
SRCS = $(wildcard $(SRC_DIR)/*.c)
OBJS = $(SRCS:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
OUT = $(BUILD_DIR)/app

all:$(OUT)
	
$(OUT):$(OBJS) | $(BUILD_DIR)
	@echo "Linking $(OUT)"
	@$(CC) $(OBJS) -o $@

$(OBJ_DIR)/%.o:$(SRC_DIR)/%.c | $(OBJ_DIR)
	@echo "Compiling $<"
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

$(OBJ_DIR):
	@mkdir -p $(OBJ_DIR)

run:$(OUT)
	@echo "Running $< \n\n"
	@./$(OUT)

clean:
	rm -rf $(BUILD_DIR)