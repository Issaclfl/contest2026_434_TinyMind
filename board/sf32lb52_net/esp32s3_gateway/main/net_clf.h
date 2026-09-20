/*
 * 本地意图分类器（推理侧）—— 把一句中文/英文说法直接映射成**闭集动作 JSON**。
 *
 * 为什么是这个形态：闭集意图分类用分类器比生成式小模型更省参数、更准，而且
 * 训练侧（tools/train_intent_clf.py）与导出侧（tools/export_clf_header.py）都是
 * 我们自己的，没有外部依赖、没有许可证问题。权重压在 clf_weights.h 里（int8）。
 *
 * 与板子的关系：输出仍是动作 JSON，网关对板子的接口不变 —— 板子一行都不用改。
 *
 * Copyright (C) 2026 TinyMind
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

/* 返回识别出的动作 JSON（静态字符串，来自 CLF_LABELS）；空输入返回 NULL。 */
const char *net_clf_classify(const char *text);

/* 模型规模，开机日志里打一行，便于核对跑的是哪一版。 */
int net_clf_classes(void);
int net_clf_dim(void);
