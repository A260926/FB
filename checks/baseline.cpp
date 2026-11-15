#include <atomic>
#include <iostream>
#include "Render/Software/RendererSoft.h"
#include "Render/Software/ShaderProgramSoft.h"
#include "Render/Software/UniformSoft.h"
using namespace SoftGL;
namespace Smoke {
struct ShaderDefines {};
struct ShaderAttributes {glm::vec4 position;glm::vec2 uv;};
struct ShaderUniforms {glm::vec4 tint;Sampler2DSoft<RGBA>* image;};
struct ShaderVaryings {glm::vec2 uv;};
static std::atomic<unsigned> fragments{0};
class Common: public ShaderSoft {
public:
 CREATE_SHADER_OVERRIDE
 std::vector<std::string>& getDefines() override {static std::vector<std::string>x;return x;}
 std::vector<UniformDesc>& getUniformsDesc() override {
  static std::vector<UniformDesc>x={{"tint",offsetof(ShaderUniforms,tint)},{"image",offsetof(ShaderUniforms,image)}};return x;
 }
};
class VS:public Common {public: CREATE_SHADER_CLONE(VS)
 void shaderMain() override {gl->Position=a->position;v->uv=a->uv;}
};
class FS:public Common {public: CREATE_SHADER_CLONE(FS)
 void shaderMain() override {++fragments;gl->FragColor=textureLod(u->image,v->uv,0.f)*u->tint;}
};
}
int main(){
 RendererSoft r;
 TextureDesc cd;cd.width=16;cd.height=16;cd.usage=TextureUsage_AttachmentColor;
 auto color=r.createTexture(cd);color->initImageData();
 TextureDesc dd=cd;dd.format=TextureFormat_FLOAT32;dd.usage=TextureUsage_AttachmentDepth;
 auto depth=r.createTexture(dd);depth->initImageData();
 auto fbo=r.createFrameBuffer(true);fbo->setColorAttachment(color,0);fbo->setDepthAttachment(depth);
 TextureDesc td;td.width=4;td.height=4;td.useMipmaps=true;
 auto tex=r.createTexture(td);
 auto pixels=Buffer<RGBA>::makeDefault(4,4);pixels->setAll(RGBA(160,80,40,255));tex->setImageData({pixels});
 SamplerDesc sd;sd.filterMin=Filter_NEAREST_MIPMAP_NEAREST;tex->setSamplerDesc(sd);
 auto program=r.createShaderProgram();
 auto ps=std::dynamic_pointer_cast<ShaderProgramSoft>(program);
 ps->SetShaders(std::make_shared<Smoke::VS>(),std::make_shared<Smoke::FS>());
 auto resources=std::make_shared<ShaderResources>();
 auto block=r.createUniformBlock("tint",sizeof(glm::vec4));glm::vec4 tint(1);block->setData(&tint,sizeof(tint));resources->blocks[0]=block;
 auto sampler=r.createUniformSampler("image",td);sampler->setTexture(tex);resources->samplers[1]=sampler;
 Smoke::ShaderAttributes vertices[]={{{-.8f,-.8f,.5f,1},{0,0}},{{.8f,-.8f,.5f,1},{1,0}},{{0,.8f,.5f,1},{.5f,1}}};
 int32_t indices[]={0,1,2};VertexArray va;va.vertexesDesc={{4,sizeof(vertices[0]),0}};
 va.vertexesBuffer=reinterpret_cast<uint8_t*>(vertices);va.vertexesBufferLength=sizeof(vertices);va.indexBuffer=indices;va.indexBufferLength=sizeof(indices);
 auto vao=r.createVertexArrayObject(va);RenderStates states;states.cullFace=false;states.depthTest=true;
 auto pipeline=r.createPipelineStates(states);r.setViewPort(0,0,16,16);r.setShaderProgram(program);r.setShaderResources(resources);r.setVertexArrayObject(vao);r.setPipelineStates(pipeline);
 ClearStates clear;clear.colorFlag=true;clear.depthFlag=true;clear.clearDepth=1;
 r.beginRenderPass(fbo,clear);r.draw();r.endRenderPass();
 auto ct=std::dynamic_pointer_cast<TextureSoft<RGBA>>(color);auto dt=std::dynamic_pointer_cast<TextureSoft<float>>(depth);
 auto p=*ct->getImage().getBuffer()->buffer->get(8,8);auto z=*dt->getImage().getBuffer()->buffer->get(8,8);
 if(std::abs(int(p.r)-160)>1||std::abs(int(p.g)-80)>1||std::abs(int(p.b)-40)>1||std::abs(z-.5f)>1e-6f||Smoke::fragments.load()==0)return 1;
 auto first=Smoke::fragments.load();for(auto& v:vertices)v.position.z=.8f;vao->updateVertexData(vertices,sizeof(vertices));r.draw();
 if(Smoke::fragments.load()!=first)return 2;
 tint=glm::vec4(.5f,.5f,.5f,1);block->setData(&tint,sizeof(tint));r.setShaderResources(resources);r.beginRenderPass(fbo,clear);r.draw();r.endRenderPass();
 p=*ct->getImage().getBuffer()->buffer->get(8,8);
 if(std::abs(int(p.r)-80)>1||std::abs(int(p.g)-40)>1||std::abs(int(p.b)-20)>1)return 3;
 std::cout<<"TEXTURED_DRAW_OK fragments="<<first<<" recolor="<<int(p.r)<<","<<int(p.g)<<","<<int(p.b)<<" depth="<<z<<" mip_levels="<<std::dynamic_pointer_cast<TextureSoft<RGBA>>(tex)->getImage().levels.size()<<"\n";
}
