公开接口约定（脚手架，不含裁剪实现）

在 ufbx.h 增加 `ufbx_extract_opts`，字段为 `ufbx_allocator_opts temp_allocator, result_allocator` 和 `uint32_t reserved`（必须为0），opts=NULL 等同全零默认值。allocator 的配置与错误回收遵循原有同类接口。

增加 `ufbx_extract_map`，字段为 `ufbx_uint32_list element_ids, node_ids`。两个数组长度分别为 source->elements.count、source->nodes.count，按源编号索引，值为结果编号或 UINT32_MAX。map 可为NULL；非NULL时失败不改，成功数组的内存归返回scene，释放scene后不再访问map。

声明：
`ufbx_abi ufbx_scene *ufbx_extract_scene(const ufbx_scene *source, const uint32_t *node_ids, size_t num_nodes, const ufbx_extract_opts *opts, ufbx_extract_map *map, ufbx_error *error);`

实现仍放 ufbx.c，C99可编译。source必须为本库加载/求值得到的有效场景；输入为node typed_id集合，num_nodes>0，node_ids非NULL。重复/重叠允许。源scene不变，结果未经过额外时间求值，保持来源当前姿态。后续调用使用结果自己的scene.anim或stack->anim，不支持把源自定义override anim直接套到新编号。

闭包算法的“祖先”和“后代”只指node层级边；共享mesh的instances为反向引用，过滤结果内成员，不作为扩展边。所有保留node的属性、材质和skin依赖反复闭包，新增依赖node不自动展开全部后代。节点绑定的pose对象保留并过滤记录；其他无关pose丢弃。支持载入时自动生成的普通node/helper，不可仅因有pivot或继承模式而拒绝。node.all_attribs 中必须按同一规则保留支持的mesh/bone/empty属性。场景中的相机和灯不在本次支持范围，只有闭包需要时才报错。

动画保留全部stack/layer容器，筛选anim_props的目标，再保留实际被使用的anim_value及对应curve；其余元数据对象、选择集、音频和显示层不进入结果。不能为了连接闭合把所有元素都保留。与被删除对象的连接必须同时从scene和element两端移除，并重建原生排序及按名/按类型的索引。对所有公开元素指针，NULL保持NULL，非NULL必须指向结果保留元素；props.defaults等不可变payload可共享原存储，但其中的元素引用也要重定向。scene.dom_root允许清空。

retained source存储只用于不可变payload寿命，原元素不能冒充结果元素。无贴图材质可有标准内建shader类型；显式自定义shader对象/texture连接不支持。未支持元素只在真正进入依赖闭包时失败，不要求拒绝整个源文件。保留的原生动画层权重和morph中间shape行为不能被重置。
