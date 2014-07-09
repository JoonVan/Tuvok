#include "callperformer.h"
#include "dirent.h"
#include "uvfDataset.h"
#include <limits.h>
#include "DebugOut/debug.h"
#include "GLGridLeaper.h"
#include "ContextIdentification.h"
#include "BatchContext.h"
#include "LuaScripting/TuvokSpecific/LuaDatasetProxy.h"
#include "RenderRegion.h"
using tuvok::DynamicBrickingDS;
using tuvok::UVFDataset;

#define SHADER_PATH "Shaders"

DECLARE_CHANNEL(dataset);
DECLARE_CHANNEL(renderer);
DECLARE_CHANNEL(file);

CallPerformer::CallPerformer()
:maxBatchSize(defaultBatchSize)
{
}

CallPerformer::~CallPerformer() {
    invalidateRenderer();
}

void CallPerformer::invalidateRenderer() {
    std::shared_ptr<tuvok::LuaScripting> ss = tuvok::Controller::Instance().LuaScript();
    if (rendererInst.isValid(ss)) {
        ss->cexec(rendererInst.fqName() + ".cleanup");
        tuvok::Controller::Instance().ReleaseVolumeRenderer(rendererInst);
        rendererInst.invalidate();
    }
    if (dsInst.isValid(ss)) {
        dsInst.invalidate();
    }
}

DynamicBrickingDS* CallPerformer::getDataSet() {
    std::shared_ptr<tuvok::LuaScripting> ss = tuvok::Controller::Instance().LuaScript();
    if(!dsInst.isValid(ss))
        return NULL;

    tuvok::LuaDatasetProxy* ds = dsInst.getRawPointer<tuvok::LuaDatasetProxy>(ss);
    return (DynamicBrickingDS*)ds->getDataset();
}

AbstrRenderer* CallPerformer::getRenderer() {
    std::shared_ptr<tuvok::LuaScripting> ss = tuvok::Controller::Instance().LuaScript();
    if(!rendererInst.isValid(ss))
        return NULL;

    return rendererInst.getRawPointer<tuvok::AbstrRenderer>(ss);
}

//File handling
vector<std::string> CallPerformer::listFiles() {
    const char* folder = getenv("IV3D_FILES_FOLDER");
    if(folder == NULL) {
        folder = "./";
    }

    vector<std::string> retVector;
    std::string extension = ".uvf";

    DIR *dir;
    struct dirent *ent;

    if ((dir = opendir (folder)) != NULL) {
        printf("\nFound the following files in folder %s:\n", folder);
        /* print all the files and directories within directory */
        while ((ent = readdir (dir)) != NULL) {

            //No directories
            if(ent->d_type != DT_DIR) {
                std::string fname = ent->d_name;

                //printf("%s\n", fname.c_str());

                //Only files ending in .uvf
                if (fname.find(extension, (fname.length() - extension.length())) != std::string::npos) {
                    std::string tmp_string = ent->d_name;
                    printf("%s,\n", tmp_string.c_str());
                    retVector.push_back(tmp_string);
                }
            }
        }
        printf("End of list!\n");
        closedir (dir);
    } else {
        /* could not open directory */
        perror("Could not open folder for uvf files!");
        abort();
    }

    return retVector;
}

std::shared_ptr<tuvok::Context> createContext(uint32_t width, uint32_t height,
                                            int32_t color_bits,
                                            int32_t depth_bits,
                                            int32_t stencil_bits,
                                            bool double_buffer, bool visible)
{
  std::shared_ptr<tuvok::BatchContext> ctx(
      tuvok::BatchContext::Create(width,height, color_bits,depth_bits,stencil_bits,
                           double_buffer,visible));
  if(!ctx->isValid() || ctx->makeCurrent() == false)
  {
    std::cerr << "Could not utilize context.";
    return std::shared_ptr<tuvok::BatchContext>();
  }

  return ctx;
}

bool CallPerformer::openFile(const std::string& filename, const std::vector<size_t>& bSize, size_t minmaxMode) {
    const char* folder = getenv("IV3D_FILES_FOLDER");
    if(folder == NULL) {
        folder = "./";
    }

    std::string effectiveFilename = folder;
    if(effectiveFilename.back() != '/') {
        effectiveFilename.append("/");
    }
    effectiveFilename.append(filename);

    //Create renderer
    std::shared_ptr<tuvok::LuaScripting> ss = tuvok::Controller::Instance().LuaScript();

    rendererInst = ss->cexecRet<tuvok::LuaClassInstance>(
                "tuvok.renderer.new",
                tuvok::MasterController::OPENGL_GRIDLEAPER, true, false,
                false, false);

    if(!rendererInst.isValid(ss)) {
        abort(); //@TODO: properly handle
    }

    std::string rn = rendererInst.fqName();

    char buff[200];

    //Hook up dataset to renderer
    UINTVECTOR3 maxBS(bSize[0], bSize[1], bSize[2]);
    UINTVECTOR2 res(width, height);

    //Dirty hack because the lua binding cannot work with MATRIX or VECTOR
    sprintf(buff, "{%d, %d, %d}", maxBS.x, maxBS.y, maxBS.z);
    std::string maxBSStr(buff);
    sprintf(buff, "{%d, %d}", res.x, res.y);
    std::string resStr(buff);

    TRACE(file, "Opening file: %s\n", effectiveFilename.c_str());
    ss->cexec(rn+".loadDataset", effectiveFilename.c_str());

    sprintf(buff, "%s.loadRebricked(\"%s\", %s, %zu)",
            rn.c_str(),
            effectiveFilename.c_str(),
            maxBSStr.c_str(),
            minmaxMode);
    std::string loadRebrickedString(buff);

    printf("Load rebricked string: %s\n", loadRebrickedString.c_str());
    //FIXME(file, "This call claims that there would be no such file when using full path on OSX...dafuq");
    if(!ss->execRet<bool>(loadRebrickedString)) {
        return false;
    }

    dsInst = ss->cexecRet<tuvok::LuaClassInstance>(rn+".getDataset");
    ss->cexec(rn+".addShaderPath", SHADER_PATH);

    //Create openGL-context
    std::shared_ptr<tuvok::Context> ctx = createContext(width, height, 32, 24, 8, true, false);

    //Render init
    //getRenderer()->Initialize(ctx);
    ss->cexec(rn+".initialize", ctx);
    ss->exec(rn+".resize("+resStr+")");
    //ss->cexec(rn+".setRendererTarget", tuvok::AbstrRenderer::RT_HEADLESS); //From CMDRenderer.cpp... but no idea why
    ss->cexec(rn+".paint");

    return true;
}

void CallPerformer::closeFile(const std::string& filename) {
    (void)filename; /// @TODO: keep it around to see which file to close?

    invalidateRenderer();
}

void CallPerformer::rotate(const float *matrix) {
    std::shared_ptr<tuvok::LuaScripting> ss = tuvok::Controller::Instance().LuaScript();
    if(!rendererInst.isValid(ss)) {
        WARN(renderer, "No renderer created! Aborting request.");
        return;
    }

#if 0
    //const FLOATMATRIX4 rotation(matrix);
    //Dirty hack because the lua binding cannot work with MATRIX or VECTOR
    std::string matString = "{";
    for(size_t i = 0; i < 15; i++) {
        matString += std::to_string(matrix[i]) + ", ";
    }
    matString += std::to_string(matrix[15]);
    matString += "}";

    const std::string rn = rendererInst.fqName();
    FIXME(renderer, "For some reason we cannot retrieve the renderRegion...");
    tuvok::LuaClassInstance renderRegion = ss->cexecRet<tuvok::LuaClassInstance>(rn+".getFirst3DRenderRegion");
    ss->exec(renderRegion.fqName()+".setRotation4x4("+matString+")");
#else
    AbstrRenderer* ren = rendererInst.getRawPointer<AbstrRenderer>(ss);
    std::shared_ptr<tuvok::RenderRegion3D> rr3d = ren->GetFirst3DRegion();
    ren->SetRotationRR(rr3d.get(), FLOATMATRIX4(matrix));
    const std::string rn = rendererInst.fqName();
#endif
    ss->cexec(rn+".paint");
}

std::vector<tuvok::BrickKey> CallPerformer::getRenderedBrickKeys() {
    std::shared_ptr<tuvok::LuaScripting> ss = tuvok::Controller::Instance().LuaScript();
    if(!rendererInst.isValid(ss) || !dsInst.isValid(ss)) {
        WARN(renderer, "Renderer or DataSet not initialized! Aborting request.");
        return std::vector<tuvok::BrickKey>(0);
    }

    //Retrieve a list of bricks that need to be send to the client
    AbstrRenderer* tmp_ren = getRenderer();
    const tuvok::GLGridLeaper* glren = dynamic_cast<tuvok::GLGridLeaper*>(tmp_ren);
    assert(glren && "not a grid leaper?  wrong renderer in us?");
    const std::vector<UINTVECTOR4> hash = glren->GetNeededBricks();
    const tuvok::LinearIndexDataset& linearDS = dynamic_cast<const tuvok::LinearIndexDataset&>(*getDataSet());

    std::vector<tuvok::BrickKey> allKeys(0);
    for(UINTVECTOR4 b : hash) {
        //printf("Test: x:%d y:%d z:%d, w:%d\n", b.x, b.y, b.z, b.w);
        allKeys.push_back(linearDS.IndexFrom4D(b, 0));
    }

    return allKeys;
}