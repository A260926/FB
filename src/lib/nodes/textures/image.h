//
//  image.h
//  c-ray
//
//  Created by Valtteri Koskivuori on 06/12/2020.
//  Copyright © 2020-2023 Valtteri Koskivuori. All rights reserved.
//

#pragma once

struct node_storage;
struct texture_asset;

const struct colorNode *newImageTexture(const struct node_storage *s, const struct texture_asset *asset, uint8_t options);
